CC=gcc
CFLAGS=-D_GNU_SOURCE -Wall -Wextra -Wpedantic -std=gnu11 -O2 -Icommon
PREFIX=/usr/local

# 主要產物：kernel dm-target module（真實工作流 = kernel + dmsetup）
all: module

# Unit tests
test_map: tests/test_map.c tests/test_common.h
	$(CC) $(CFLAGS) -o $@ $<

# Compile the REAL kernel driver source (driver/tieredvol_stripe.c) against
# minimal mock kernel headers (tests/mock/linux/) and run it in userspace.
# This validates that pure-math helpers and the parallel completion/timeout
# handoff behave as expected without a kernel.
test_stripe_kernel: tests/test_stripe_kernel.c tests/test_common.h tests/mock/linux/*.h driver/tieredvol.h driver/tieredvol_stripe.c
	$(CC) $(CFLAGS) -O1 -Itests/mock -Idriver -Wno-unused-parameter -o $@ tests/test_stripe_kernel.c

test: test_map test_stripe_kernel
	@TP=0; TR=0; \
	for t in test_map test_stripe_kernel; do \
		echo "=== $$t ===" && ./$$t; \
		P=$$?; \
		if [ $$P -eq 0 ]; then TP=$$((TP+1)); fi; \
		TR=$$((TR+1)); \
	done; \
	echo ""; \
	echo "=== Suites: $$TP/$$TR passed ==="; \
	if [ $$TP -ne $$TR ]; then exit 1; fi

test-full: test
	@TV_DEVS=$$(ls /dev/mapper/tv_* 2>/dev/null | head -1); \
	if [ -z "$$TV_DEVS" ]; then \
		echo "=== test-full ==="; \
		echo "  SKIP  no /dev/mapper/tv_* device found (create one with: dmsetup create, see docs/CONFIG.md)"; \
	else \
		echo "=== test-full: fio on $$TV_DEVS ==="; \
		fio --name=write --filename=$$TV_DEVS --rw=write --bs=2m --direct=1 --ioengine=libaio --iodepth=256 --size=128M --numjobs=1 --runtime=10 --time_based --group_reporting 2>&1 | tail -3; \
		fio --name=read --filename=$$TV_DEVS --rw=read --bs=2m --direct=1 --ioengine=libaio --iodepth=256 --size=128M --numjobs=1 --runtime=10 --time_based --group_reporting 2>&1 | tail -3; \
	fi

lint:
	@echo "=== Lint: syntax check ===" && \
	errors=0; \
	for f in tests/test_map.c tests/test_stripe_kernel.c; do \
		if ! gcc -fsyntax-only -Wall -Wextra -Wpedantic -std=gnu11 -D_GNU_SOURCE -Icommon -Itests -Itests/mock -Idriver -Wno-unused-parameter $$f 2>&1; then errors=$$((errors+1)); fi; \
	done; \
	echo "=== Lint: $$errors file(s) with issues ==="; \
	if [ $$errors -ne 0 ]; then exit 1; fi

# Kernel module targets
module:
	make -C /lib/modules/$(shell uname -r)/build M=$(CURDIR)/driver modules

module_install:
	make -C /lib/modules/$(shell uname -r)/build M=$(CURDIR)/driver modules_install
	depmod -a

module_clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(CURDIR)/driver clean

# install / uninstall 以 kernel module 為主要產物（需 root）
install: module_install
	@echo ""
	@echo "Installed: tieredvol.ko (dm target)"
	@echo ""

uninstall:
	rm -f $(DESTDIR)/lib/modules/$(shell uname -r)/extra/tieredvol.ko
	rm -f $(DESTDIR)/lib/modules/$(shell uname -r)/updates/tieredvol.ko
	depmod -a

clean:
	rm -f test_map test_stripe_kernel
	make -C /lib/modules/$(shell uname -r)/build M=$(CURDIR)/driver clean

.PHONY: all install uninstall clean test test-full lint module module_install module_clean
