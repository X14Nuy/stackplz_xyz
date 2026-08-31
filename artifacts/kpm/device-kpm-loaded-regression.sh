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
  if [ -n "${SPZPID:-}" ]; then kill -15 "$SPZPID" 2>/dev/null || true; fi
  if [ -n "${PID:-}" ]; then
    umount "/proc/$PID" 2>/dev/null || true
    kill -9 "$PID" 2>/dev/null || true
  fi
  "$KP" kpm ctl0 stackplz-kpm clear >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

run_case() {
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
    if ! kill -0 "$SPZPID" 2>/dev/null; then break; fi
    sleep 1
    ATTACH_WAIT=$((ATTACH_WAIT + 1))
  done
  if [ "$READY" -eq 1 ]; then sleep "$SAMPLE_SECONDS"; fi
  kill -15 "$SPZPID" 2>/dev/null || true
  wait "$SPZPID" 2>/dev/null || true
  SPZPID=""
  TARGET_LINES=$(grep -c write_loop "$LABEL.log" 2>/dev/null || true)
  HIT_LINES=$(grep -c kpm_hit "$LABEL.log" 2>/dev/null || true)
  POLL_ERRORS=$(grep -c kpm_poll_error "$LABEL.log" 2>/dev/null || true)
  echo "$LABEL ready=$READY attach_wait=${ATTACH_WAIT}s target=$TARGET_LINES hits=$HIT_LINES poll_errors=$POLL_ERRORS bytes=$(wc -c < "$LABEL.log")"
  grep -m 2 'selected task source\|kpm_bound\|write_loop' "$LABEL.log" 2>/dev/null || true
  cat "$LABEL.stdout"
}

"$KP" kpm ctl0 stackplz-kpm clear >/dev/null 2>&1 || true
./write_loop >/dev/null 2>&1 &
PID=$!
sleep 1
cp "/proc/$PID/maps" write_loop.maps
mount -o bind "$EMPTY" "/proc/$PID"
chmod 000 "$EMPTY" 2>/dev/null || true

echo "TARGET_PID=$PID"
echo "PS_LINES=$(ps -p "$PID" | wc -l)"

run_case loaded_auto_syscall 2 --pid "$PID" --task-source auto --kpm-profile "$PROFILE" --syscall write --showuid
run_case loaded_auto_uprobe 1 --pid "$PID" --task-source auto --kpm-profile "$PROFILE" --lib libc.so --point 'write[int,buf:1,int]' --regs --showuid
run_case loaded_auto_perf_brk 1 --pid "$PID" --task-source auto --kpm-profile "$PROFILE" --maps-file write_loop.maps --brk "0x$WRITE_OFF:x" --brk-lib libc.so --brk-backend perf --regs --showuid
run_case loaded_direct_first 1 --pid "$PID" --task-source kpm --kpm-profile "$PROFILE" --maps-file write_loop.maps --brk "0x$WRITE_OFF:x" --brk-lib libc.so --brk-backend kpm-direct --brk-mode repeat --regs --showuid
run_case loaded_direct_second 1 --pid "$PID" --task-source kpm --kpm-profile "$PROFILE" --maps-file write_loop.maps --brk "0x$WRITE_OFF:x" --brk-lib libc.so --brk-backend kpm-direct --brk-mode repeat --regs --showuid

echo FINAL_STATUS
"$KP" kpm ctl0 stackplz-kpm status
echo KPM_LOADED_REGRESSION_DONE
