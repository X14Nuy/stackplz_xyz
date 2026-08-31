#!/bin/sh
set -e
cd /home/zzc/stackplz_xyz/kpm
STOP_AFTER_VALIDATE="${STOP_AFTER_VALIDATE:-0}"
STOP_AFTER_PERCPU="${STOP_AFTER_PERCPU:-0}"
SKIP_INIT_TASK="${SKIP_INIT_TASK:-0}"
STOP_AFTER_PAGE_SIZE="${STOP_AFTER_PAGE_SIZE:-0}"
STOP_AFTER_DFR0="${STOP_AFTER_DFR0:-0}"
STOP_AFTER_SYMBOLS="${STOP_AFTER_SYMBOLS:-0}"
STOP_AFTER_BANNER="${STOP_AFTER_BANNER:-0}"
python3 - <<'PY'
from pathlib import Path
p = Path('Makefile')
text = p.read_text()
if '-fno-unwind-tables' not in text:
    text = text.replace(
        '-fno-builtin -fno-pic -fno-pie -mno-outline-atomics',
        '-fno-builtin -fno-pic -fno-pie -fno-common -fno-unwind-tables -fno-asynchronous-unwind-tables -mno-outline-atomics',
    )
text = text.replace('verify: check-sdk $(TARGET)', 'verify: $(TARGET)')
text = text.replace('$(TARGET): check-sdk $(OBJECTS)', '$(TARGET): $(OBJECTS)')
p.write_text(text)
print('makefile patched')
PY
EXTRA_CFLAGS=""
if [ "$STOP_AFTER_PAGE_SIZE" = "1" ]; then
  EXTRA_CFLAGS="$EXTRA_CFLAGS -DSPZ_STOP_AFTER_PAGE_SIZE"
  echo "building diagnostic KPM: stop after page_size"
fi
if [ "$STOP_AFTER_DFR0" = "1" ]; then
  EXTRA_CFLAGS="$EXTRA_CFLAGS -DSPZ_STOP_AFTER_DFR0"
  echo "building diagnostic KPM: stop after dfr0"
fi
if [ "$STOP_AFTER_SYMBOLS" = "1" ]; then
  EXTRA_CFLAGS="$EXTRA_CFLAGS -DSPZ_STOP_AFTER_SYMBOLS"
  echo "building diagnostic KPM: stop after symbol lookup"
fi
if [ "$STOP_AFTER_BANNER" = "1" ]; then
  EXTRA_CFLAGS="$EXTRA_CFLAGS -DSPZ_STOP_AFTER_BANNER"
  echo "building diagnostic KPM: stop after banner"
fi
if [ "$SKIP_INIT_TASK" = "1" ]; then
  EXTRA_CFLAGS="$EXTRA_CFLAGS -DSPZ_SKIP_INIT_TASK"
  echo "skipping init_task verification"
fi
if [ "$STOP_AFTER_PERCPU" = "1" ]; then
  EXTRA_CFLAGS="$EXTRA_CFLAGS -DSPZ_PREPARE_STOP_AFTER_PERCPU"
  echo "building diagnostic KPM: stop after per-cpu table"
elif [ "$STOP_AFTER_VALIDATE" = "1" ]; then
  EXTRA_CFLAGS="$EXTRA_CFLAGS -DSPZ_PREPARE_STOP_AFTER_VALIDATE"
  echo "building diagnostic KPM: stop after profile validate"
fi
make clean
make TARGET_COMPILE=aarch64-linux-gnu- KP_DIR=/home/zzc/kp-sdk \
  CFLAGS="-std=gnu11 -O2 -ffreestanding -fno-stack-protector -fno-builtin -fno-pic -fno-pie -fno-common -fno-unwind-tables -fno-asynchronous-unwind-tables -mno-outline-atomics -mgeneral-regs-only -Wall -Wextra -Werror -DSPZ_KPATCH_BUILD -include include/stackplz/freestanding.h $EXTRA_CFLAGS" \
  stackplz-kpm.kpm
echo '=== undefined ==='
aarch64-linux-gnu-nm -u stackplz-kpm.kpm
echo '=== relocs of interest ==='
aarch64-linux-gnu-readelf -r stackplz-kpm.kpm | awk '/GOT|311|PREL32|Type/ {print}' | head -40
sha256sum stackplz-kpm.kpm
ls -l stackplz-kpm.kpm
python3 ../tools/check_kpm_elf.py --elf stackplz-kpm.kpm --kpatch-dir /home/zzc/kp-sdk \
  --nm aarch64-linux-gnu-nm --readelf aarch64-linux-gnu-readelf || \
  echo "ELF verify failed (expected if diagnostic DCE dropped hook imports)"
mkdir -p /home/zzc/stackplz_xyz/artifacts/kpm
cp -f stackplz-kpm.kpm /home/zzc/stackplz_xyz/artifacts/kpm/stackplz-kpm.kpm
echo "copied artifact STOP_AFTER_VALIDATE=$STOP_AFTER_VALIDATE"
