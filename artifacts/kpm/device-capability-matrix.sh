#!/system/bin/sh
set -u

KP=/data/adb/modules/KPatch-Next/bin/kpatch
DIR=/data/local/tmp/stackplz-kpm-test
PROFILE=oneplus-plk110-a16-b4999618-d05
EMPTY=$DIR/empty_proc
WRITE_OFF=ef700

cd "$DIR" || exit 1
mkdir -p "$EMPTY"
chmod 0755 "$EMPTY"

cleanup() {
  chmod 0755 "$EMPTY" 2>/dev/null || true
  if [ -n "${PID:-}" ]; then
    umount "/proc/$PID" 2>/dev/null || true
    kill -9 "$PID" 2>/dev/null || true
  fi
  if [ -n "${SPZPID:-}" ]; then
    kill -15 "$SPZPID" 2>/dev/null || true
  fi
  "$KP" kpm ctl0 stackplz-kpm "clear" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

run_spz() {
  LABEL=$1
  SAMPLE_SECONDS=$2
  shift 2
  rm -f "$LABEL.log" "$LABEL.stdout"
  ./stackplz "$@" --btf --nocheck --quiet -o "$LABEL.log" >"$LABEL.stdout" 2>&1 &
  SPZPID=$!
  READY=0
  ATTACH_WAIT=0
  while [ "$ATTACH_WAIT" -lt 30 ]; do
    if grep -q 'start [0-9][0-9]* modules' "$LABEL.log" 2>/dev/null; then
      READY=1
      break
    fi
    if ! kill -0 "$SPZPID" 2>/dev/null; then
      break
    fi
    sleep 1
    ATTACH_WAIT=$((ATTACH_WAIT + 1))
  done
  if [ "$READY" -eq 1 ]; then
    sleep "$SAMPLE_SECONDS"
  fi
  kill -15 "$SPZPID" 2>/dev/null || true
  wait "$SPZPID" 2>/dev/null || true
  SPZPID=""
  echo "=== $LABEL stdout ==="
  cat "$LABEL.stdout"
  echo "=== $LABEL summary ==="
  echo "ready=$READY attach_wait=${ATTACH_WAIT}s sample=${SAMPLE_SECONDS}s bytes=$(wc -c < "$LABEL.log") lines=$(wc -l < "$LABEL.log") target_lines=$(grep -c "write_loop" "$LABEL.log" 2>/dev/null || true) write_lines=$(grep -c "write" "$LABEL.log" 2>/dev/null || true)"
  head -5 "$LABEL.log" 2>/dev/null || true
  tail -5 "$LABEL.log" 2>/dev/null || true
  echo "=== $LABEL end ==="
}

"$KP" kpm ctl0 stackplz-kpm "clear" >/dev/null 2>&1 || true

./write_loop >/dev/null 2>&1 &
PID=$!
sleep 0.5
echo "TARGET_PID=$PID"
ps -p "$PID" | head -3
cp "/proc/$PID/maps" write_loop.maps
echo "maps_bytes=$(wc -c < write_loop.maps)"
grep 'libc.so' write_loop.maps | head -3
readelf -Ws /apex/com.android.runtime/lib64/bionic/libc.so 2>/dev/null | grep -E '[[:space:]]write(@@|$)' | head -3 || true

run_spz visible_syscall 2 --pid "$PID" --task-source proc --syscall write --showuid
run_spz visible_uprobe 1 --pid "$PID" --task-source proc --lib libc.so --point 'write[int,buf:1,int]' --stack --regs --showuid
run_spz visible_perf_brk 1 --pid "$PID" --task-source proc --brk "0x$WRITE_OFF:x" --brk-lib libc.so --brk-backend perf --stack --regs --showuid

echo "=== hide target ==="
mount -o bind "$EMPTY" "/proc/$PID"
chmod 000 "$EMPTY" 2>/dev/null || true
ps -p "$PID" | head -3

echo "=== hidden proc source must fail ==="
./stackplz --pid "$PID" --task-source proc --syscall write --btf --nocheck --quiet -o hidden_proc.log >hidden_proc.stdout 2>&1
echo "hidden_proc_rc=$?"
cat hidden_proc.stdout

run_spz hidden_kpm_syscall 2 --pid "$PID" --task-source kpm --kpm-profile "$PROFILE" --syscall write --showuid
run_spz hidden_kpm_uprobe 1 --pid "$PID" --task-source kpm --kpm-profile "$PROFILE" --lib libc.so --point 'write[int,buf:1,int]' --stack --regs --showuid

echo "=== hidden KPM brk-lib without maps must report current limitation ==="
./stackplz --pid "$PID" --task-source kpm --kpm-profile "$PROFILE" --brk "0x$WRITE_OFF:x" --brk-lib libc.so --brk-backend perf --btf --nocheck --quiet -o hidden_no_maps.log >hidden_no_maps.stdout 2>&1
echo "hidden_no_maps_rc=$?"
cat hidden_no_maps.stdout

run_spz hidden_kpm_perf_brk 1 --pid "$PID" --task-source kpm --kpm-profile "$PROFILE" --maps-file write_loop.maps --brk "0x$WRITE_OFF:x" --brk-lib libc.so --brk-backend perf --stack --regs --showuid
run_spz hidden_kpm_direct_brk 2 --pid "$PID" --task-source kpm --kpm-profile "$PROFILE" --maps-file write_loop.maps --brk "0x$WRITE_OFF:x" --brk-lib libc.so --brk-backend kpm-direct --brk-mode repeat --regs --showuid

echo "=== final KPM status ==="
"$KP" kpm ctl0 stackplz-kpm "status"
echo CAPABILITY_MATRIX_DONE
