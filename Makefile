CC=gcc
CFLAGS=-Wall -Wextra -Wpedantic -std=gnu11 -O2
PREFIX=/usr/local

all: tiered_setup tiered_ui

tiered_setup: src/tiered_setup.c
	$(CC) $(CFLAGS) -o $@ $<

tiered_ui: src/tiered_ui.c
	$(CC) $(CFLAGS) -o $@ $< -lncurses

install: all
	install -m 755 tiered_setup $(DESTDIR)$(PREFIX)/bin/tiered_setup
	install -m 755 tiered_ui $(DESTDIR)$(PREFIX)/bin/tiered_ui
	mkdir -p $(DESTDIR)/etc/tieredvol

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/tiered_setup
	rm -f $(DESTDIR)$(PREFIX)/bin/tiered_ui

clean:
	rm -f tiered_setup tiered_ui

.PHONY: all install uninstall clean
