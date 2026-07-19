CC=gcc
CFLAGS=-Wall -Wextra -Wpedantic -std=gnu11 -O2
PREFIX=/usr/local

SCHED_OBJS=src/tiered_sched.o src/tiered_partition.o src/tiered_mapper.o \
           src/tiered_io_uring.o src/tiered_metadata.o \
           src/tiered_benchmark.o

all: tiered_setup tiered_ui tiered_io

tiered_setup: src/tiered_setup.c src/tiered_common.h src/tiered_sched.h src/version.h $(SCHED_OBJS)
	$(CC) $(CFLAGS) -o $@ src/tiered_setup.c $(SCHED_OBJS) -lm -luring

tiered_ui: src/tiered_ui.c src/tiered_common.h src/tiered_ui_helpers.h src/tiered_sched.h src/version.h $(SCHED_OBJS)
	$(CC) $(CFLAGS) -o $@ src/tiered_ui.c $(SCHED_OBJS) -lncurses -luring

tiered_io: src/tiered_io.c src/tiered_sched.h $(SCHED_OBJS)
	$(CC) $(CFLAGS) -o $@ src/tiered_io.c $(SCHED_OBJS) -luring

src/tiered_sched.o: src/tiered_sched.c src/tiered_sched.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/tiered_partition.o: src/tiered_partition.c src/tiered_sched.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/tiered_mapper.o: src/tiered_mapper.c src/tiered_sched.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/tiered_io_uring.o: src/tiered_io_uring.c src/tiered_sched.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/tiered_metadata.o: src/tiered_metadata.c src/tiered_sched.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/tiered_benchmark.o: src/tiered_benchmark.c src/tiered_sched.h
	$(CC) $(CFLAGS) -c -o $@ $<

test_common: tests/test_common.c src/tiered_common.h
	$(CC) $(CFLAGS) -o $@ $<

test_tui: tests/test_tui.c src/tiered_ui_helpers.h
	$(CC) $(CFLAGS) -o $@ $< -lm

test: test_tui test_common
	./test_tui && ./test_common

install: all
	install -m 755 tiered_setup $(DESTDIR)$(PREFIX)/bin/tiered_setup
	install -m 755 tiered_ui $(DESTDIR)$(PREFIX)/bin/tiered_ui
	install -m 755 tiered_io $(DESTDIR)$(PREFIX)/bin/tiered_io
	install -m 755 scripts/tieredvol-restore.sh $(DESTDIR)$(PREFIX)/bin/tieredvol-restore.sh
	mkdir -p $(DESTDIR)/etc/tieredvol
	mkdir -p $(DESTDIR)/etc/systemd/system
	install -m 644 scripts/tieredvol-restore.service $(DESTDIR)/etc/systemd/system/tieredvol-restore.service
	@echo ""
	@echo "Installed:"
	@echo "  $(DESTDIR)$(PREFIX)/bin/tiered_setup"
	@echo "  $(DESTDIR)$(PREFIX)/bin/tiered_ui"
	@echo "  $(DESTDIR)$(PREFIX)/bin/tiered_io"
	@echo "  $(DESTDIR)$(PREFIX)/bin/tieredvol-restore.sh"
	@echo "  $(DESTDIR)/etc/systemd/system/tieredvol-restore.service"
	@echo ""
	@echo "To enable auto-restore on boot:"
	@echo "  sudo systemctl daemon-reload"
	@echo "  sudo systemctl enable tieredvol-restore"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/tiered_setup
	rm -f $(DESTDIR)$(PREFIX)/bin/tiered_ui
	rm -f $(DESTDIR)$(PREFIX)/bin/tiered_io
	rm -f $(DESTDIR)$(PREFIX)/bin/tieredvol-restore.sh
	rm -f $(DESTDIR)/etc/systemd/system/tieredvol-restore.service

clean:
	rm -f tiered_setup tiered_ui tiered_io test_tui test_common
	rm -f src/*.o

.PHONY: all install uninstall clean test
