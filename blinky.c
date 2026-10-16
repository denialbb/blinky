// blinky - ThinkPad ThinkLight status indicator for coding agents.
//
// One LED (tpacpi::thinklight) is binary: max_brightness == 1.  There is no
// hardware dimming and no hardware blink.  To fake a dimming heartbeat we run
// software PWM and vary the duty cycle.  The achievable carrier frequency is
// bounded by how fast we can poke the LED through thinkpad_acpi (one ACPI call
// per toggle), so we calibrate at startup and fall back to plain on/off
// patterns when PWM is too slow to look like dimming.
//
// Control surface: a FIFO.  `blinky set <agent> <state>` appends one line; the
// daemon keeps per-agent state and renders the most urgent one.
//
// Build: make.  Run: ./blinky daemon   (or let `blinky set` auto-start it).

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DEFAULT_LED "/sys/class/leds/tpacpi::thinklight/brightness"
#define MAX_AGENTS 64
#define LINE_MAX_ 512

/* ---------------------------------------------------------------- timing */

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void sleep_ms(double ms)
{
	if (ms <= 0)
		return;
	struct timespec ts;
	ts.tv_sec = (time_t)(ms / 1000.0);
	ts.tv_nsec = (long)((ms - (double)ts.tv_sec * 1000.0) * 1e6);
	while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
		;
}

/* ---------------------------------------------------------------- patterns */

// Pattern ids, ordered by urgency: higher wins.
enum pattern {
	P_OFF = 0,
	P_DONE = 1,
	P_RUNNING = 2,
	P_HEARTBEAT = 3,
	P_ERROR = 4,
	P_NEEDS = 5,
};

static const char *pattern_name(int p)
{
	switch (p) {
	case P_OFF: return "off";
	case P_DONE: return "done";
	case P_RUNNING: return "running";
	case P_HEARTBEAT: return "heartbeat";
	case P_ERROR: return "error";
	case P_NEEDS: return "needs-input";
	}
	return "?";
}

static double clamp01(double v)
{
	return v < 0 ? 0 : (v > 1 ? 1 : v);
}

// One thump: fast attack, exponential decay.  x is ms since beat start.
static double beat(double x)
{
	const double attack = 28.0; /* ms */
	const double decay = 170.0; /* ms time constant */
	if (x < 0)
		return 0;
	if (x < attack)
		return x / attack;
	return exp(-(x - attack) / decay);
}

// Two-thump cardiac envelope, ~55 bpm.  Returns 0..1 duty.
static double heartbeat(double t_ms)
{
	const double period = 1100.0;
	double u = fmod(t_ms, period);
	double v = beat(u) + 0.72 * beat(u - 300.0);
	return clamp01(v);
}

// Slow breathing for "working".
static double breath(double t_ms)
{
	const double period = 2600.0;
	double u = fmod(t_ms, period);
	return 0.5 - 0.5 * cos(2.0 * M_PI * u / period);
}

// Triple blink for "waiting for you".
static double needs_blink(double t_ms)
{
	double u = fmod(t_ms, 1000.0);
	if (u < 90 || (u >= 190 && u < 280) || (u >= 380 && u < 470))
		return 1.0;
	return 0.0;
}

// Three flashes for "done", then dark.
static double done_flash(double t_ms)
{
	if (t_ms < 0 || t_ms > 3000.0)
		return 0.0;
	double u = fmod(t_ms, 260.0);
	return u < 130 ? 1.0 : 0.0;
}

// SOS in Morse (dot 140ms, dash 420ms), then a pause.
static double sos(double t_ms)
{
	// (on_ms, off_ms) pairs
	static const double seg[] = {
		140, 140, 140, 140, 140, 420,  /* ... */
		420, 140, 420, 140, 420, 420,  /* --- */
		140, 140, 140, 140, 140, 1400, /* ... */
	};
	const int n = (int)(sizeof(seg) / sizeof(seg[0]));
	double period = 0;
	for (int i = 0; i < n; i++)
		period += seg[i];
	double u = fmod(t_ms, period);
	for (int i = 0; i < n; i += 2) {
		if (u < seg[i])
			return 1.0;
		u -= seg[i];
		if (u < seg[i + 1])
			return 0.0;
		u -= seg[i + 1];
	}
	return 0.0;
}

static double pattern_duty(int p, double t_ms, bool pwm)
{
	double d;
	switch (p) {
	case P_OFF: return 0.0;
	case P_RUNNING: d = pwm ? 0.10 + 0.90 * breath(t_ms) : (breath(t_ms) > 0.35 ? 1.0 : 0.0); break;
	case P_HEARTBEAT: d = pwm ? 0.03 + 0.97 * heartbeat(t_ms) : (heartbeat(t_ms) > 0.18 ? 1.0 : 0.0); break;
	case P_NEEDS: d = needs_blink(t_ms); break;
	case P_DONE: d = done_flash(t_ms); break;
	case P_ERROR: d = sos(t_ms); break;
	default: d = 0.0; break;
	}
	return clamp01(d);
}

/* --------------------------------------------------------------------- led */

struct led {
	int fd;
	int shadow; /* last written value, -1 unknown */
	const char *path;
	bool warned;
};

static void led_write(struct led *l, int on)
{
	if (l->fd < 0)
		return;
	on = on ? 1 : 0;
	if (l->shadow == on)
		return;
	// sysfs attributes must be written at offset 0 every time.
	(void)!lseek(l->fd, 0, SEEK_SET);
	const char *s = on ? "1\n" : "0\n";
	if (write(l->fd, s, 2) < 0) {
		if (!l->warned) {
			fprintf(stderr, "blinky: write %s: %s\n", l->path, strerror(errno));
			l->warned = true;
		}
		return;
	}
	l->shadow = on;
}

// Measure toggles/second of the LED path.  Also proves write access.
static double led_calibrate(struct led *l)
{
	const int n = 400;
	uint64_t t0, t1;
	for (int i = 0; i < 16; i++) { /* warm up */
		led_write(l, 1);
		led_write(l, 0);
	}
	t0 = now_ns();
	for (int i = 0; i < n; i++) {
		led_write(l, 1);
		led_write(l, 0);
	}
	t1 = now_ns();
	led_write(l, 0);
	double sec = (double)(t1 - t0) / 1e9;
	if (sec <= 0)
		return 0;
	return (double)(2 * n) / sec;
}

/* ------------------------------------------------------------- state store */

enum { A_OFF = 0, A_RUNNING, A_NEEDS, A_DONE, A_ERROR };

struct agent {
	char name[40];
	int state;
};

struct world {
	struct agent agents[MAX_AGENTS];
	int nagents;
	int forced;      /* -1 = none, else forced pattern */
	bool paused;
	struct led led;
};

static int agent_state_from_name(const char *s)
{
	if (!strcmp(s, "running") || !strcmp(s, "working") || !strcmp(s, "busy"))
		return A_RUNNING;
	if (!strcmp(s, "needs-input") || !strcmp(s, "blocked") || !strcmp(s, "waiting"))
		return A_NEEDS;
	if (!strcmp(s, "done") || !strcmp(s, "idle") || !strcmp(s, "settled"))
		return A_DONE;
	if (!strcmp(s, "error") || !strcmp(s, "failed"))
		return A_ERROR;
	return A_OFF;
}

static int pattern_from_name(const char *s)
{
	if (!strcmp(s, "off")) return P_OFF;
	if (!strcmp(s, "done")) return P_DONE;
	if (!strcmp(s, "running")) return P_RUNNING;
	if (!strcmp(s, "heartbeat")) return P_HEARTBEAT;
	if (!strcmp(s, "error") || !strcmp(s, "sos")) return P_ERROR;
	if (!strcmp(s, "needs-input") || !strcmp(s, "needs")) return P_NEEDS;
	return -1;
}

static void world_set_agent(struct world *w, const char *name, int state)
{
	for (int i = 0; i < w->nagents; i++) {
		if (!strcmp(w->agents[i].name, name)) {
			w->agents[i].state = state;
			return;
		}
	}
	if (w->nagents < MAX_AGENTS) {
		snprintf(w->agents[w->nagents].name, sizeof(w->agents[0].name), "%s", name);
		w->agents[w->nagents].state = state;
		w->nagents++;
	}
}

static int world_dominant(const struct world *w)
{
	if (w->forced >= 0)
		return w->forced;
	if (w->paused)
		return P_OFF;
	int best = P_OFF;
	for (int i = 0; i < w->nagents; i++) {
		int p = P_OFF;
		switch (w->agents[i].state) {
		case A_RUNNING: p = P_RUNNING; break;
		case A_NEEDS: p = P_NEEDS; break;
		case A_DONE: p = P_DONE; break;
		case A_ERROR: p = P_ERROR; break;
		default: p = P_OFF; break;
		}
		if (p > best)
			best = p;
	}
	return best;
}

/* ---------------------------------------------------------------- daemon io */

struct paths {
	char dir[512];
	char fifo[600];
	char pid[600];
	char status[600];
	char log[600];
	char calib[600];
};

static void make_paths(struct paths *p)
{
	const char *rt = getenv("XDG_RUNTIME_DIR");
	const char *base = getenv("BLINKY_BASE");
	if (base && *base)
		rt = base;
	char root[512];
	if (rt && *rt)
		snprintf(root, sizeof(root), "%s/blinky", rt);
	else
		snprintf(root, sizeof(root), "/tmp/blinky-%d", (int)getuid());
	snprintf(p->dir, sizeof(p->dir), "%s", root);
	snprintf(p->fifo, sizeof(p->fifo), "%s/cmd", p->dir);
	snprintf(p->pid, sizeof(p->pid), "%s/pid", p->dir);
	snprintf(p->status, sizeof(p->status), "%s/status", p->dir);
	snprintf(p->log, sizeof(p->log), "%s/log", p->dir);
	snprintf(p->calib, sizeof(p->calib), "%s/calib", p->dir);
}

static bool dir_ready(const struct paths *p)
{
	if (mkdir(p->dir, S_IRWXU) == 0)
		return true;
	return errno == EEXIST;
}

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static pid_t read_pid(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return 0;
	long v = 0;
	if (fscanf(f, "%ld", &v) != 1)
		v = 0;
	fclose(f);
	return (pid_t)v;
}

static bool pid_alive(pid_t pid)
{
	return pid > 0 && (kill(pid, 0) == 0 || errno == EPERM);
}

static void handle_command(struct world *w, char *line)
{
	char *save = NULL;
	char *verb = strtok_r(line, " \t\r\n", &save);
	if (!verb)
		return;
	if (!strcmp(verb, "set")) {
		char *agent = strtok_r(NULL, " \t\r\n", &save);
		char *state = strtok_r(NULL, " \t\r\n", &save);
		if (agent && state) {
			w->forced = -1; /* a real update cancels any manual pattern */
			world_set_agent(w, agent, agent_state_from_name(state));
		}
	} else if (!strcmp(verb, "clear") || !strcmp(verb, "off")) {
		char *agent = strtok_r(NULL, " \t\r\n", &save);
		if (agent)
			world_set_agent(w, agent, A_OFF);
		else {
			w->nagents = 0;
			w->forced = -1;
		}
	} else if (!strcmp(verb, "pause")) {
		w->paused = true;
	} else if (!strcmp(verb, "resume")) {
		w->paused = false;
	} else if (!strcmp(verb, "pattern")) {
		char *name = strtok_r(NULL, " \t\r\n", &save);
		if (name)
			w->forced = pattern_from_name(name);
	} else if (!strcmp(verb, "quit")) {
		g_stop = 1;
	}
}

static void write_status(const struct paths *p, const struct world *w, int dom, bool pwm, double hz)
{
	char buf[256];
	snprintf(buf, sizeof(buf), "%s pwm=%s max_toggle_hz=%.0f agents=%d\n",
		 pattern_name(dom), pwm ? "on" : "off", hz, w->nagents);
	int fd = open(p->status, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
	if (fd >= 0) {
		(void)!write(fd, buf, strlen(buf));
		close(fd);
	}
}

static int cmd_daemon(void)
{
	struct paths p;
	make_paths(&p);
	if (!dir_ready(&p)) {
		fprintf(stderr, "blinky: cannot create %s: %s\n", p.dir, strerror(errno));
		return 1;
	}
	pid_t old = read_pid(p.pid);
	if (pid_alive(old) && old != getpid()) {
		fprintf(stderr, "blinky: daemon already running (pid %ld)\n", (long)old);
		return 0;
	}

	const char *ledpath = getenv("BLINKY_LED");
	if (!ledpath || !*ledpath)
		ledpath = DEFAULT_LED;

	struct world w;
	memset(&w, 0, sizeof(w));
	w.forced = -1;
	struct led *l = &w.led;
	l->fd = open(ledpath, O_WRONLY);
	l->shadow = -1;
	l->path = ledpath;
	if (l->fd < 0) {
		fprintf(stderr,
			"blinky: cannot open %s: %s\n"
			"        run ./install.sh once (root) to grant access via udev.\n",
			ledpath, strerror(errno));
		return 1;
	}

	// Measuring flashes the lamp ~400 times, so cache the result per machine.
	double hz = 0;
	bool recal = getenv("BLINKY_RECALIBRATE") != NULL;
	if (!recal) {
		FILE *cf = fopen(p.calib, "r");
		if (cf) {
			double v = 0;
			if (fscanf(cf, "%lf", &v) == 1 && v > 0)
				hz = v;
			fclose(cf);
		}
	}
	if (hz <= 0) {
		hz = led_calibrate(l);
		FILE *cf = fopen(p.calib, "w");
		if (cf) {
			fprintf(cf, "%.3f\n", hz);
			fclose(cf);
		}
	} else {
		led_write(l, 1);
		led_write(l, 0);
	}
	const char *pm = getenv("BLINKY_PWM");
	bool want_pwm = !(pm && (!strcmp(pm, "0") || !strcmp(pm, "off") || !strcmp(pm, "no")));
	double carrier = 100.0;
	const char *cs = getenv("BLINKY_PWM_HZ");
	if (cs && atof(cs) > 0)
		carrier = atof(cs);
	// Two writes per carrier period; keep a 1.5x margin.
	bool pwm = want_pwm && hz >= 2.0 * carrier * 1.5;

	// FIFO
	unlink(p.fifo);
	if (mkfifo(p.fifo, S_IRUSR | S_IWUSR) < 0) {
		fprintf(stderr, "blinky: mkfifo %s: %s\n", p.fifo, strerror(errno));
		return 1;
	}
	int fifo = open(p.fifo, O_RDONLY | O_NONBLOCK);
	if (fifo < 0) {
		fprintf(stderr, "blinky: open fifo: %s\n", strerror(errno));
		return 1;
	}

	FILE *pf = fopen(p.pid, "w");
	if (pf) {
		fprintf(pf, "%ld\n", (long)getpid());
		fclose(pf);
	}

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGPIPE, SIG_IGN);

	fprintf(stderr,
		"blinky: led=%s max_toggle=%.0fHz pwm=%s (carrier %.0fHz)\n",
		ledpath, hz, pwm ? "yes" : "no", carrier);

	const uint64_t frame_ns = (uint64_t)(1e9 / carrier);
	double frame_ms = (double)frame_ns / 1e6;
	uint64_t next = now_ns();
	int last_dom = -1;
	uint64_t t0 = now_ns();
	char buf[LINE_MAX_ * 4];
	size_t used = 0;

	while (!g_stop) {
		struct pollfd pfd = { .fd = fifo, .events = POLLIN };
		int64_t wait_ms = ((int64_t)next - (int64_t)now_ns()) / 1000000;
		if (wait_ms < 0)
			wait_ms = 0;
		poll(&pfd, 1, (int)wait_ms);

		// drain commands
		ssize_t n = read(fifo, buf + used, sizeof(buf) - used - 1);
		if (n > 0) {
			used += (size_t)n;
			buf[used] = '\0';
			char *start = buf;
			char *nl;
			while ((nl = strchr(start, '\n')) != NULL) {
				*nl = '\0';
				handle_command(&w, start);
				start = nl + 1;
			}
			size_t left = strlen(start);
			memmove(buf, start, left);
			used = left;
		}

		if (g_stop)
			break;
		if (now_ns() < next)
			continue; /* woke early for a command; no frame yet */

		int dom = world_dominant(&w);
		if (dom != last_dom) {
			t0 = now_ns();
			last_dom = dom;
			write_status(&p, &w, dom, pwm, hz);
		}
		double t_ms = (double)(now_ns() - t0) / 1e6;
		double duty = pattern_duty(dom, t_ms, pwm);

		if (!pwm) {
			led_write(l, duty > 0.5);
		} else {
			double on_ms = duty * frame_ms;
			if (on_ms <= 0.4) {
				led_write(l, 0);
				sleep_ms(frame_ms);
			} else if (on_ms >= frame_ms - 0.4) {
				led_write(l, 1);
				sleep_ms(frame_ms);
			} else {
				led_write(l, 1);
				sleep_ms(on_ms);
				led_write(l, 0);
				sleep_ms(frame_ms - on_ms);
			}
		}

		next += frame_ns;
		if ((int64_t)next < (int64_t)now_ns())
			next = now_ns();
	}

	led_write(l, 0);
	if (l->fd >= 0)
		close(l->fd);
	close(fifo);
	unlink(p.fifo);
	unlink(p.pid);
	fprintf(stderr, "blinky: stopped\n");
	return 0;
}

/* ---------------------------------------------------------------------- cli */

static int send_line(const char *line, bool autostart)
{
	struct paths p;
	make_paths(&p);
	if (!dir_ready(&p))
		return 1;

	for (int attempt = 0; attempt < 100; attempt++) {
		int fd = open(p.fifo, O_WRONLY | O_NONBLOCK);
		if (fd >= 0) {
			size_t n = strlen(line);
			(void)!write(fd, line, n);
			close(fd);
			return 0;
		}
		if (errno != ENXIO && errno != ENOENT)
			break;
		if (attempt == 0 && autostart) {
			// spawn a detached daemon
			pid_t pid = fork();
			if (pid == 0) {
				setsid();
				int devnull = open("/dev/null", O_RDWR);
				if (devnull >= 0) {
					(void)!dup2(devnull, STDIN_FILENO);
					(void)!dup2(devnull, STDOUT_FILENO);
					(void)!dup2(devnull, STDERR_FILENO);
				}
				int lf = open(p.log, O_WRONLY | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR);
				if (lf >= 0)
					dup2(lf, STDERR_FILENO);
				execl("/proc/self/exe", "blinky", "daemon", (char *)NULL);
				_exit(127);
			}
		}
		sleep_ms(50);
	}
	fprintf(stderr, "blinky: daemon not running and could not be started\n");
	return 1;
}

static int cmd_stop(void)
{
	struct paths p;
	make_paths(&p);
	pid_t pid = read_pid(p.pid);
	if (!pid_alive(pid)) {
		printf("blinky: not running\n");
		return 0;
	}
	kill(pid, SIGTERM);
	for (int i = 0; i < 40 && pid_alive(pid); i++)
		sleep_ms(50);
	printf("blinky: stopped\n");
	return 0;
}

static int cmd_status(void)
{
	struct paths p;
	make_paths(&p);
	pid_t pid = read_pid(p.pid);
	printf("daemon: %s%s\n", pid_alive(pid) ? "running" : "stopped",
	       pid_alive(pid) ? "" : "");
	char buf[512];
	int fd = open(p.status, O_RDONLY);
	if (fd >= 0) {
		ssize_t n = read(fd, buf, sizeof(buf) - 1);
		if (n > 0) {
			buf[n] = '\0';
			printf("status: %s", buf);
		}
		close(fd);
	}
	return 0;
}

static int cmd_led(int argc, char **argv)
{
	if (argc < 1)
		return 1;
	const char *ledpath = getenv("BLINKY_LED");
	if (!ledpath || !*ledpath)
		ledpath = DEFAULT_LED;
	struct led l = { .fd = open(ledpath, O_WRONLY), .shadow = -1, .path = ledpath };
	if (l.fd < 0) {
		fprintf(stderr, "blinky: cannot open %s: %s\n", ledpath, strerror(errno));
		return 1;
	}
	led_write(&l, !strcmp(argv[0], "on") || !strcmp(argv[0], "1"));
	close(l.fd);
	return 0;
}

static int cmd_calibrate(void)
{
	const char *ledpath = getenv("BLINKY_LED");
	if (!ledpath || !*ledpath)
		ledpath = DEFAULT_LED;
	struct led l = { .fd = open(ledpath, O_WRONLY), .shadow = -1, .path = ledpath };
	if (l.fd < 0) {
		fprintf(stderr, "blinky: cannot open %s: %s\n", ledpath, strerror(errno));
		return 1;
	}
	double hz = led_calibrate(&l);
	close(l.fd);
	printf("%.0f toggles/sec on %s\n", hz, ledpath);
	printf("pwm@100Hz: %s\n", hz >= 300 ? "yes" : "no (binary patterns only)");
	return 0;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: blinky <command> [args]\n"
		"  set <agent> <state>   running|needs-input|done|error|off\n"
		"  pattern <name>        off|running|heartbeat|needs-input|done|error\n"
		"  off [agent]           clear one agent, or all\n"
		"  pause | resume\n"
		"  status | start | stop\n"
		"  led on|off            raw write, bypasses the daemon\n"
		"  calibrate             measure max toggle rate / PWM feasibility\n"
		"  daemon                run in the foreground\n"
		"env: BLINKY_LED BLINKY_BASE BLINKY_PWM BLINKY_PWM_HZ\n");
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		usage();
		return 2;
	}
	const char *cmd = argv[1];
	if (!strcmp(cmd, "daemon"))
		return cmd_daemon();
	if (!strcmp(cmd, "calibrate"))
		return cmd_calibrate();
	if (!strcmp(cmd, "led"))
		return cmd_led(argc - 2, argv + 2);
	if (!strcmp(cmd, "status"))
		return cmd_status();
	if (!strcmp(cmd, "stop"))
		return cmd_stop();
	if (!strcmp(cmd, "start"))
		return send_line("", true) ? 1 : 0; /* empty line is ignored, wakes daemon */

	if (!strcmp(cmd, "set")) {
		if (argc < 4) {
			usage();
			return 2;
		}
		char line[LINE_MAX_];
		snprintf(line, sizeof(line), "set %s %s\n", argv[2], argv[3]);
		return send_line(line, true);
	}
	if (!strcmp(cmd, "pattern")) {
		if (argc < 3) {
			usage();
			return 2;
		}
		char line[LINE_MAX_];
		snprintf(line, sizeof(line), "pattern %s\n", argv[2]);
		return send_line(line, true);
	}
	if (!strcmp(cmd, "off")) {
		char line[LINE_MAX_];
		if (argc >= 3)
			snprintf(line, sizeof(line), "off %s\n", argv[2]);
		else
			snprintf(line, sizeof(line), "off\n");
		return send_line(line, false);
	}
	if (!strcmp(cmd, "pause"))
		return send_line("pause\n", false);
	if (!strcmp(cmd, "resume"))
		return send_line("resume\n", false);

	usage();
	return 2;
}
