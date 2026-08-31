CC ?= cc
CROSS_COMPILE ?= aarch64-linux-gnu-
CROSS_CC := $(CROSS_COMPILE)gcc

MODEL_STEPS ?= 100000
CPPFLAGS := -I../include -I../generated -DSPZ_MODEL_STEPS=$(MODEL_STEPS)
CFLAGS := -std=c11 -O2 -g -Wall -Wextra -Werror -Wpedantic -pthread
SUITES := test_command test_ring test_profile test_task test_debug test_arm64_encoding \
	test_hooks test_async test_control test_maps test_maps_renderer
SELECTED := $(if $(strip $(TEST)),$(TEST),$(SUITES))
BINARIES := $(addprefix build/,$(SELECTED))
CORE_SOURCES := ../core/command.c ../core/ring.c ../core/crc32.c ../core/maps.c \
	../core/profile.c ../core/task.c ../core/debug.c \
	../platform/arm64/debug_regs.c ../platform/kpatch/hooks.c \
	../platform/kpatch/async.c ../platform/kpatch/runtime.c \
	../platform/kpatch/control.c ../platform/kpatch/maps.c
CORE_HEADERS := ../include/stackplz/abi.h ../include/stackplz/core.h ../include/stackplz/maps.h \
	../include/stackplz/profile.h ../include/stackplz/task.h \
	../include/stackplz/debug.h ../generated/device_profiles.h \
	../platform/arm64/debug_regs.h ../platform/kpatch/compat.h
TEST_SUPPORT_SOURCES := mock_debug_regs.c

.PHONY: all test sanitize arm64-objects clean

all: test

test: $(BINARIES)
	@set -e; for binary in $(BINARIES); do ./$$binary; done

build/%: test_main.c %.c test.h $(CORE_SOURCES) $(CORE_HEADERS) $(TEST_SUPPORT_SOURCES)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) test_main.c $*.c $(TEST_SUPPORT_SOURCES) $(CORE_SOURCES) -o $@

sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CFLAGS) -O1 -fno-omit-frame-pointer -fsanitize=address,undefined" TEST="$(SELECTED)" test

arm64-objects: build/arm64/debug_regs.o build/arm64/test_arm64_encoding.o

build/arm64/debug_regs.o: ../platform/arm64/debug_regs.c ../platform/arm64/debug_regs.h $(CORE_HEADERS)
	@mkdir -p build/arm64
	$(CROSS_CC) -I../include -I../generated -std=gnu11 -O2 -ffreestanding \
		-mgeneral-regs-only -Wall -Wextra -Werror -c $< -o $@

build/arm64/test_arm64_encoding.o: test_arm64_encoding.c ../platform/arm64/debug_regs.h $(CORE_HEADERS)
	@mkdir -p build/arm64
	$(CROSS_CC) -I../include -I../generated -std=gnu11 -O2 -ffreestanding \
		-mgeneral-regs-only -Wall -Wextra -Werror -c $< -o $@

clean:
	rm -rf build
