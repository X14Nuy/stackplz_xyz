ifneq ($(MAKECMDGOALS),clean)
ifndef TARGET_COMPILE
$(error TARGET_COMPILE not set)
endif

ifndef KP_DIR
$(error KP_DIR not set)
endif
endif

CC := $(TARGET_COMPILE)gcc
NM := $(TARGET_COMPILE)nm
READELF := $(TARGET_COMPILE)readelf
PYTHON ?= python3
EXPECTED_KP_COMMIT := 0fe6d142266b80e5aa445a7ea1534f88a8f33a35

KP_INCLUDE_DIRS := . include patch/include linux/include \
	linux/arch/arm64/include linux/tools/arch/arm64/include
KP_INCLUDE_FLAGS := $(foreach dir,$(KP_INCLUDE_DIRS),-I$(KP_DIR)/kernel/$(dir))
LOCAL_INCLUDE_FLAGS := -Iinclude -Igenerated -Iplatform/arm64 -Iplatform/kpatch
CFLAGS ?= -std=gnu11 -O2 -ffreestanding -fno-stack-protector \
	-fno-builtin -fno-pic -fno-pie -fno-common \
	-fno-unwind-tables -fno-asynchronous-unwind-tables \
	-mno-outline-atomics -mgeneral-regs-only \
	-Wall -Wextra -Werror -DSPZ_KPATCH_BUILD \
	-include include/stackplz/freestanding.h

SOURCES := core/command.c core/ring.c core/crc32.c core/maps.c core/profile.c core/task.c \
	core/debug.c platform/arm64/debug_regs.c platform/kpatch/hooks.c \
	platform/kpatch/async.c platform/kpatch/runtime.c \
	platform/kpatch/control.c platform/kpatch/maps.c platform/kpatch/builtins.c \
	platform/kpatch/main.c
OBJECTS := $(addprefix build/,$(SOURCES:.c=.o))
TARGET := stackplz-kpm.kpm

.PHONY: all check-sdk verify clean

all: verify

check-sdk:
	@test -f "$(KP_DIR)/kernel/patch/module/module.c"
	@test "$$(git -C "$(KP_DIR)" rev-parse HEAD)" = "$(EXPECTED_KP_COMMIT)" || \
		{ echo "KPatch SDK commit does not match $(EXPECTED_KP_COMMIT)"; exit 1; }
	@grep -q "kpm_safe_unload_v1" "$(KP_DIR)/kernel/patch/module/module.c" || \
		{ echo "KPatch SDK lacks kpm/patches/kpatch-next-d05-safe-kpm-unload.patch"; exit 1; }

verify: check-sdk $(TARGET)
	$(PYTHON) ../tools/check_kpm_elf.py --elf $(TARGET) \
		--kpatch-dir "$(KP_DIR)" --nm $(NM) --readelf $(READELF)

$(TARGET): check-sdk $(OBJECTS)
	$(CC) -r -o $@ $(OBJECTS)

build/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(KP_INCLUDE_FLAGS) $(LOCAL_INCLUDE_FLAGS) -c $< -o $@

clean:
	rm -rf build $(TARGET)
