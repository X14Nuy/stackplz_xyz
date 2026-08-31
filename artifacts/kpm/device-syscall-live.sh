#!/system/bin/sh
set -u

DIR=/data/local/tmp/stackplz-kpm-test
cd "$DIR" || exit 1

cleanup() {
  if [ -n "${SPZPID:-}" ]; then kill -15 "$SPZPID" 2>/dev/null || true; fi
  if [ -n "${PID:-}" ]; then kill -9 "$PID" 2>/dev/null || true; fi
}
trap cleanup EXIT INT TERM

./write_loop >/dev/null 2>&1 &
PID=$!
./stackplz --pid "$PID" --task-source proc --syscall all --nocheck --quiet --debug -o live-syscall.log >live-syscall.stdout 2>&1 &
SPZPID=$!
echo "TARGET_PID=$PID STACKPLZ_PID=$SPZPID"
sleep 60
kill -15 "$SPZPID" 2>/dev/null || true
wait "$SPZPID" 2>/dev/null || true
SPZPID=""
echo SYSCALL_LIVE_DONE
