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

echo "=== load KPM ==="
$KP kpm load "$DIR/stackplz-kpm.kpm" "profile=$PROFILE"
echo "load_exit=$?"
echo "=== dmesg load ==="
dmesg | grep -E "stackplz-kpm|unknown symbol|load_module" | tail -15
$KP kpm list
echo "=== info ==="
$KP kpm info stackplz-kpm
echo "=== status ==="
$KP kpm ctl0 stackplz-kpm "status"
echo "=== profile ctl ==="
$KP kpm ctl0 stackplz-kpm "profile"

sleep 600 &
PID=$!
sleep 0.4
echo "=== target pid=$PID (independent spawn) ==="
echo "BEFORE_HIDE ps:"
ps -p "$PID" 2>&1 | head -5
echo "BEFORE_HIDE uid:"
ps -o uid= -p "$PID" 2>&1
echo "BEFORE_HIDE status Name:"
sed -n '1,8p' "/proc/$PID/status" 2>&1

echo "=== hide /proc/$PID ==="
mount -o bind "$EMPTY" "/proc/$PID"
echo "mount_exit=$?"
chmod 000 "$EMPTY" 2>/dev/null || true
echo "AFTER_HIDE ls:"
ls -ld "/proc/$PID" 2>&1 | head -2
ls "/proc/$PID" 2>&1 | head -3
echo "AFTER_HIDE ps:"
ps -p "$PID" 2>&1 | head -5
echo "AFTER_HIDE uid:"
ps -o uid= -p "$PID" 2>&1
echo "AFTER_HIDE status:"
cat "/proc/$PID/status" 2>&1 | head -3

echo "=== proc source (expect fail) ==="
./stackplz --pid "$PID" --task-source proc --syscall nanosleep --nocheck -o hidden-proc.log
echo "proc_exit=$?"
tail -5 hidden-proc.log 2>/dev/null

echo "=== kpm source (expect bind + events) ==="
./stackplz --pid "$PID" --task-source kpm --kpm-profile "$PROFILE" --syscall nanosleep --nocheck -o hidden-kpm.log --quiet > hidden-kpm.stdout 2>&1 &
SPZPID=$!
sleep 6
kill "$SPZPID" 2>/dev/null || true
wait "$SPZPID" 2>/dev/null || true
SPZPID=""
echo "=== kpm stdout ==="
cat hidden-kpm.stdout
echo "kpm_log_bytes=$(wc -c < hidden-kpm.log)"
echo "=== kpm log head ==="
head -20 hidden-kpm.log
echo "=== kpm nanosleep count ==="
grep -c nanosleep hidden-kpm.log 2>/dev/null || echo 0
echo "=== kpm log tail ==="
tail -15 hidden-kpm.log

echo "=== kpm status after run ==="
$KP kpm ctl0 stackplz-kpm "status"

cleanup
echo "=== unload ==="
$KP kpm ctl0 stackplz-kpm "clear" || true
sleep 0.3
$KP kpm ctl0 stackplz-kpm "status" || true
$KP kpm unload stackplz-kpm
echo "unload_exit=$?"
$KP kpm num
echo "=== leftover ==="
ps -A 2>/dev/null | grep stackplz | grep -v grep || echo none
echo DONE
