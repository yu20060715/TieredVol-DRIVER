CC=gcc
CFLAGS=-Wall -Wextra -Wpedantic -std=gnu11 -O2
PREFIX=/usr/local

all: tiered_setup tiered_ui

tiered_setup: src/tiered_setup.c src/tiered_common.h src/version.h
	$(CC) $(CFLAGS) -o $@ $< -lm

tiered_ui: src/tiered_ui.c src/tiered_common.h src/tiered_ui_helpers.h src/version.h
	$(CC) $(CFLAGS) -o $@ $< -lncurses

test_common: tests/test_common.c src/tiered_common.h
	$(CC) $(CFLAGS) -o $@ $<

test_tui: tests/test_tui.c src/tiered_ui_helpers.h
	$(CC) $(CFLAGS) -o $@ $< -lm

test: test_tui test_common
	./test_tui && ./test_common

install: all
	install -m 755 tiered_setup $(DESTDIR)$(PREFIX)/bin/tiered_setup
	install -m 755 tiered_ui $(DESTDIR)$(PREFIX)/bin/tiered_ui
	install -m 755 scripts/tieredvol-restore.sh $(DESTDIR)$(PREFIX)/bin/tieredvol-restore.sh
	mkdir -p $(DESTDIR)/etc/tieredvol
	mkdir -p $(DESTDIR)/etc/systemd/system
	install -m 644 scripts/tieredvol-restore.service $(DESTDIR)/etc/systemd/system/tieredvol-restore.service
	@echo ""
	@echo "Installed:"
	@echo "  $(DESTDIR)$(PREFIX)/bin/tiered_setup"
	@echo "  $(DESTDIR)$(PREFIX)/bin/tiered_ui"
	@echo "  $(DESTDIR)$(PREFIX)/bin/tieredvol-restore.sh"
	@echo "  $(DESTDIR)/etc/systemd/system/tieredvol-restore.service"
	@echo ""
	@echo "To enable auto-restore on boot:"
	@echo "  sudo systemctl daemon-reload"
	@echo "  sudo systemctl enable tieredvol-restore"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/tiered_setup
	rm -f $(DESTDIR)$(PREFIX)/bin/tiered_ui
	rm -f $(DESTDIR)$(PREFIX)/bin/tieredvol-restore.sh
	rm -f $(DESTDIR)/etc/systemd/system/tieredvol-restore.service

clean:
	rm -f tiered_setup tiered_ui test_tui test_common

.PHONY: all install uninstall clean test
