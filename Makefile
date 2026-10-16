CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Wpedantic -D_GNU_SOURCE
LDLIBS ?= -lm
PREFIX ?= /usr/local

all: blinky

blinky: blinky.c
	# shellcheck disable=SC2068
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

install: blinky
	install -Dm755 blinky $(DESTDIR)$(PREFIX)/bin/blinky

# Needs root: grants userspace write access to the LED and preloads triggers.
install-udev:
	install -Dm644 udev/70-blinky.rules $(DESTDIR)/etc/udev/rules.d/70-blinky.rules
	install -Dm644 modules-load/blinky.conf $(DESTDIR)/etc/modules-load.d/blinky.conf
	udevadm control --reload
	udevadm trigger --subsystem-match=leds
	-modprobe ledtrig-timer ledtrig-pattern ledtrig-heartbeat

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/blinky

clean:
	rm -f blinky

.PHONY: all install install-udev uninstall clean
