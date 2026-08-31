#!/system/bin/sh
set -u
cd /data/local/tmp/stackplz-kpm-test
SPZ=./stackplz
EMPTY=/data/local/tmp/stackplz-kpm-test/empty_proc
mkdir -p "$EMPTY"
chmod 0755 "$EMPTY"

cleanup() {
  if [ -n "${PID:-}" ]; then
    chmod 0755 "$EMPTY" 2>/dev/null || true
    umount "/proc/$PID" 2>/dev/null || true
    kill -9 "$PID" 2>/dev/null || true
  fi
  if [ -n "${SPZPID:-}" ]; then
    kill -9 "$SPZPID" 2>/dev/null || true
  fi
}

sleep 600 &
PID=$!
sleep 0.3
echo "=== spawned pid=$PID ==="
echo "=== independent identity before hide ==="
echo "ls_proc_pid:"
ls -ld "/proc/$PID" 2>&1 | head -1
echo "status:"
sed -n '1,10p' "/proc/$PID/status" 2>&1
echo "ps_uid:"
ps -o uid= -p "$PID" 2>&1
echo "ps_p:"
ps -p "$PID" 2>&1 | head -5

echo "=== D03-like visible stackplz proc+ebpf nanosleep ==="
$SPZ --pid "$PID" --syscall nanosleep --nocheck -o visible-ebpf.log --debug &
SPZPID=$!
sleep 4
kill "$SPZPID" 2>/dev/null || true
wait "$SPZPID" 2>/dev/null || true
SPZPID=""
echo "=== visible-ebpf.log tail ==="
tail -20 visible-ebpf.log 2>/dev/null
echo "=== visible event count ==="
grep -c nanosleep visible-ebpf.log 2>/dev/null || echo 0

echo "=== hide /proc/$PID via bind-mount ==="
mount -o bind "$EMPTY" "/proc/$PID"
echo "mount_exit=$?"
chmod 000 "$EMPTY" 2>/dev/null || true
echo "ls_proc_pid_after:"
ls -ld "/proc/$PID" 2>&1 | head -2
ls "/proc/$PID" 2>&1 | head -5
echo "ps_uid_after:"
ps -o uid= -p "$PID" 2>&1
echo "ps_p_after:"
ps -p "$PID" 2>&1 | head -5
echo "cat_status_after:"
cat "/proc/$PID/status" 2>&1 | head -3

echo "=== hidden + task-source=proc no uid (expect fail) ==="
$SPZ --pid "$PID" --task-source proc --syscall nanosleep --nocheck -o hidden-proc.log --debug
echo "hidden_proc_exit=$?"
echo "=== hidden-proc.log ==="
cat hidden-proc.log 2>/dev/null

echo "=== hidden + task-source=kpm (expect fail, module missing) ==="
$SPZ --pid "$PID" --task-source kpm --syscall nanosleep --nocheck -o hidden-kpm.log --debug
echo "hidden_kpm_exit=$?"
echo "=== hidden-kpm.log ==="
cat hidden-kpm.log 2>/dev/null

echo "=== hidden + task-source=auto (expect combined error) ==="
$SPZ --pid "$PID" --task-source auto --syscall nanosleep --nocheck -o hidden-auto.log --debug
echo "hidden_auto_exit=$?"
echo "=== hidden-auto.log ==="
cat hidden-auto.log 2>/dev/null

echo "=== hidden + proc + explicit uid=0 (uid override) ==="
$SPZ --pid "$PID" --uid 0 --task-source proc --syscall nanosleep --nocheck -o hidden-uid.log --debug &
SPZPID=$!
sleep 4
kill "$SPZPID" 2>/dev/null || true
wait "$SPZPID" 2>/dev/null || true
SPZPID=""
echo "=== hidden-uid.log tail ==="
tail -30 hidden-uid.log 2>/dev/null
echo "=== hidden-uid event count ==="
grep -c nanosleep hidden-uid.log 2>/dev/null || echo 0

cleanup
echo "=== after cleanup proc visible again? ==="
# PID killed, just report
echo "done"
