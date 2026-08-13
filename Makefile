CC=gcc
CFLAGS=-D_GNU_SOURCE -Wall -Wextra -Wpedantic -std=gnu11 -O2 -Icommon
PREFIX=/usr/local

SCHED_OBJS=src/tiered_partition.o \
           src/tiered_metadata.o src/tiered_benchmark.o src/warmup.o

SETUP_OBJS=src/exec_helper.o src/setup_discover.o src/setup_bench.o src/cmd_create.o src/cmd_remove.o

all: tiered_setup

tiered_setup: src/main.c src/tiered_common.h src/tiered_types.h src/version.h src/setup_discover.h src/setup_bench.h src/exec_helper.h src/cmd_create.h src/cmd_remove.h $(SCHED_OBJS) $(SETUP_OBJS)
	$(CC) $(CFLAGS) -o $@ src/main.c $(SCHED_OBJS) $(SETUP_OBJS) -lm

src/tiered_partition.o: src/tiered_partition.c src/tiered_types.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/tiered_metadata.o: src/tiered_metadata.c src/tiered_types.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/tiered_benchmark.o: src/tiered_benchmark.c src/tiered_types.h src/warmup.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/warmup.o: src/warmup.c src/warmup.h src/tiered_types.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/exec_helper.o: src/exec_helper.c src/exec_helper.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/setup_discover.o: src/setup_discover.c src/setup_discover.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/setup_bench.o: src/setup_bench.c src/setup_bench.h src/setup_discover.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/cmd_create.o: src/cmd_create.c src/cmd_create.h src/tiered_types.h src/setup_discover.h src/setup_bench.h src/exec_helper.h src/version.h src/tiered_common.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/cmd_remove.o: src/cmd_remove.c src/cmd_remove.h src/cmd_create.h src/tiered_types.h src/setup_discover.h src/exec_helper.h src/tiered_common.h
	$(CC) $(CFLAGS) -c -o $@ $<

# Unit tests (pure logic — no kernel/liburing dependency)
test_common: tests/test_common.c src/tiered_common.h src/setup_bench.h src/setup_discover.h src/setup_bench.c src/exec_helper.c src/setup_discover.c src/tiered_benchmark.c src/warmup.c
	$(CC) $(CFLAGS) -o $@ tests/test_common.c src/setup_bench.c src/exec_helper.c src/setup_discover.c src/tiered_benchmark.c src/warmup.c -lm

test_partition: tests/test_partition.c src/tiered_types.h $(SCHED_OBJS)
	$(CC) $(CFLAGS) -o $@ $< $(SCHED_OBJS)

test_metadata: tests/test_metadata.c src/tiered_types.h $(SCHED_OBJS)
	$(CC) $(CFLAGS) -o $@ $< $(SCHED_OBJS)

test_map: tests/test_map.c tests/test_common.h
	$(CC) $(CFLAGS) -o $@ $<

test_exec: tests/test_exec.c src/exec_helper.h src/exec_helper.c
	$(CC) $(CFLAGS) -o $@ tests/test_exec.c src/exec_helper.c

# Compile the REAL kernel driver source (driver/tieredvol_stripe.c) against
# minimal mock kernel headers (tests/mock/linux/) and run it in userspace.
# This validates that pure-math helpers and the parallel completion/timeout
# handoff behave as expected without a kernel.
test_stripe_kernel: tests/test_stripe_kernel.c tests/test_common.h tests/mock/linux/*.h driver/tieredvol.h driver/tieredvol_stripe.c
	$(CC) $(CFLAGS) -O1 -Itests/mock -Idriver -Wno-unused-parameter -o $@ tests/test_stripe_kernel.c

test: test_common test_partition test_metadata test_map test_exec test_stripe_kernel
	@TP=0; TR=0; \
	for t in test_common test_partition test_metadata test_map test_exec test_stripe_kernel; do \
		echo "=== $$t ===" && ./$$t; \
		P=$$?; \
		if [ $$P -eq 0 ]; then TP=$$((TP+1)); fi; \
		TR=$$((TR+1)); \
	done; \
	echo ""; \
	echo "=== Suites: $$TP/$$TR passed ==="

test-full: test
	@TV_DEVS=$$(ls /dev/mapper/tv_* 2>/dev/null | head -1); \
	if [ -z "$$TV_DEVS" ]; then \
		echo "=== test-full ==="; \
		echo "  SKIP  no /dev/mapper/tv_* device found (create one with: sudo tiered_setup --create ...)"; \
	else \
		echo "=== test-full: fio on $$TV_DEVS ==="; \
		fio --name=write --filename=$$TV_DEVS --rw=write --bs=2m --direct=1 --ioengine=io_uring --iodepth=256 --size=128M --numjobs=1 --runtime=10 --time_based --group_reporting 2>&1 | tail -3; \
		fio --name=read --filename=$$TV_DEVS --rw=read --bs=2m --direct=1 --ioengine=io_uring --iodepth=256 --size=128M --numjobs=1 --runtime=10 --time_based --group_reporting 2>&1 | tail -3; \
	fi

lint:
	@echo "=== Lint: syntax check ===" && \
	errors=0; \
	for f in src/*.c; do \
		gcc -fsyntax-only -Wall -Wextra -Wpedantic -std=gnu11 -D_GNU_SOURCE $$f 2>&1 || errors=$$((errors+1)); \
	done; \
	echo "=== Lint: $$errors file(s) with issues ==="

# Kernel module targets
module:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD)/driver modules

module_install:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD)/driver modules_install
	depmod -a

module_clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD)/driver clean

install: all
	install -m 755 tiered_setup $(DESTDIR)$(PREFIX)/bin/tiered_setup
	mkdir -p $(DESTDIR)/etc/tieredvol
	mkdir -p $(DESTDIR)/etc/systemd/system
	@echo ""
	@echo "Installed:"
	@echo "  $(DESTDIR)$(PREFIX)/bin/tiered_setup"
	@echo ""

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/tiered_setup

clean:
	rm -f tiered_setup test_common test_partition test_metadata test_map test_exec test_stripe_kernel
	rm -f src/*.o

.PHONY: all install uninstall clean test test-full lint module module_install module_clean
