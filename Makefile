CC=gcc
CFLAGS=-Wall -Wextra -Wpedantic -std=gnu11 -O2
PREFIX=/usr/local

all: tiered_setup tiered_ui

tiered_setup: src/tiered_setup.c src/tiered_common.h
	$(CC) $(CFLAGS) -o $@ $<

tiered_ui: src/tiered_ui.c src/tiered_common.h
	$(CC) $(CFLAGS) -o $@ $< -lncurses

test_common: tests/test_common.c src/tiered_common.h
	$(CC) $(CFLAGS) -o $@ $<

test_tui: tests/test_tui.c
	$(CC) $(CFLAGS) -o $@ $< -lm

test: test_tui test_common
	./test_tui && ./test_common

install: all
	install -m 755 tiered_setup $(DESTDIR)$(PREFIX)/bin/tiered_setup
	install -m 755 tiered_ui $(DESTDIR)$(PREFIX)/bin/tiered_ui
	mkdir -p $(DESTDIR)/etc/tieredvol

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/tiered_setup
	rm -f $(DESTDIR)$(PREFIX)/bin/tiered_ui

clean:
	rm -f tiered_setup tiered_ui test_tui test_common

.PHONY: all install uninstall clean test
