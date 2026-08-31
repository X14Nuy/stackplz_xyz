#!/system/bin/sh
set -u
KP=/data/adb/modules/KPatch-Next/bin/kpatch
DIR=/data/local/tmp/stackplz-kpm-test
PROFILE=oneplus-plk110-a16-b4999618-d05
EMPTY=$DIR/empty_proc
cd "$DIR"
mkdir -p "$EMPTY"
chmod 0755 "$EMPTY"

cleanup() {
  chmod 0755 "$EMPTY" 2>/dev/null || true
  if [ -n "${PID:-}" ]; then
    umount "/proc/$PID" 2>/dev/null || true
    kill -9 "$PID" 2>/dev/null || true
  fi
  if [ -n "${SPZPID:-}" ]; then
    kill -9 "$SPZPID" 2>/dev/null || true
  fi
}

$KP kpm ctl0 stackplz-kpm "clear" >/dev/null 2>&1 || true

/system/bin/yes >/dev/null 2>&1 &
PID=$!
sleep 0.3
echo "=== target pid=$PID comm ==="
tr '\0' ' ' < /proc/$PID/cmdline; echo
ps -p "$PID" | head -3

echo "=== visible proc/write 2s control ==="
./stackplz --pid "$PID" --task-source proc --syscall write --nocheck -o visible-write.log >/dev/null 2>&1 &
SPZPID=$!
sleep 2
kill "$SPZPID" 2>/dev/null || true
wait "$SPZPID" 2>/dev/null || true
SPZPID=""
echo "visible_write_lines=$(wc -l < visible-write.log)"
grep -c write visible-write.log || echo 0
tail -3 visible-write.log

echo "=== hide /proc/$PID ==="
mount -o bind "$EMPTY" "/proc/$PID"
chmod 000 "$EMPTY" 2>/dev/null || true
echo "AFTER_HIDE ps:"
ps -p "$PID" 2>&1 | head -3

echo "=== hidden proc source (expect fail) ==="
./stackplz --pid "$PID" --task-source proc --syscall write --nocheck -o hidden-proc.log
echo "proc_exit=$?"

echo "=== hidden kpm/write 4s ==="
./stackplz --pid "$PID" --task-source kpm --kpm-profile "$PROFILE" --syscall write --nocheck -o hidden-kpm.log > hidden-kpm.stdout 2>&1 &
SPZPID=$!
sleep 4
kill "$SPZPID" 2>/dev/null || true
wait "$SPZPID" 2>/dev/null || true
SPZPID=""
echo "=== stdout ==="
cat hidden-kpm.stdout
echo "hidden_kpm_lines=$(wc -l < hidden-kpm.log)"
echo "write_hits=$(grep -c write hidden-kpm.log || echo 0)"
echo "=== log tail ==="
tail -8 hidden-kpm.log
echo "=== status ==="
$KP kpm ctl0 stackplz-kpm "status" || true

cleanup
echo HIDDEN_TEST_DONE
