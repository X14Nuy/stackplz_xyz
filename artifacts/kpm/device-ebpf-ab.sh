#!/system/bin/sh
set -u

DIR=/data/local/tmp/stackplz-kpm-test
cd "$DIR" || exit 1

cleanup() {
  if [ -n "${SPZPID:-}" ]; then
    kill -15 "$SPZPID" 2>/dev/null || true
  fi
  if [ -n "${PID:-}" ]; then
    kill -9 "$PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

run_case() {
  BINARY=$1
  LABEL=$2
  SAMPLE_SECONDS=$3
  shift 3
  rm -f "$LABEL.log" "$LABEL.stdout"
  "$BINARY" --pid "$PID" --task-source proc "$@" --nocheck --quiet -o "$LABEL.log" >"$LABEL.stdout" 2>&1 &
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
  echo "$LABEL ready=$READY attach_wait=${ATTACH_WAIT}s sample=${SAMPLE_SECONDS}s bytes=$(wc -c < "$LABEL.log") lines=$(wc -l < "$LABEL.log") target=$(grep -c write_loop "$LABEL.log" 2>/dev/null || true) self=$(grep -c stackplz "$LABEL.log" 2>/dev/null || true)"
  grep -m 2 'write_loop\|stackplz' "$LABEL.log" 2>/dev/null || true
  cat "$LABEL.stdout"
}

./write_loop >/dev/null 2>&1 &
PID=$!
sleep 0.5
echo "AB_TARGET_PID=$PID"
ps -p "$PID" | head -3

run_case ./stackplz.pre-fix ab_old_syscall 3 --syscall write --showuid
run_case ./stackplz ab_new_syscall 3 --syscall write --showuid
run_case ./stackplz.pre-fix ab_old_uprobe 2 --lib libc.so --point 'write[int,buf:1,int]' --showuid
run_case ./stackplz ab_new_uprobe 2 --lib libc.so --point 'write[int,buf:1,int]' --showuid
run_case ./stackplz.pre-fix ab_old_native_btf_syscall 3 --btf --syscall write --showuid
run_case ./stackplz ab_new_native_btf_syscall 3 --btf --syscall write --showuid
run_case ./stackplz.pre-fix ab_old_native_btf_uprobe 2 --btf --lib libc.so --point 'write[int,buf:1,int]' --showuid
run_case ./stackplz ab_new_native_btf_uprobe 2 --btf --lib libc.so --point 'write[int,buf:1,int]' --showuid

echo EBPF_AB_DONE
