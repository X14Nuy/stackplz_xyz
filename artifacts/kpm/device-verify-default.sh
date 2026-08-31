#!/system/bin/sh
set -u
KP=/data/adb/modules/KPatch-Next/bin/kpatch
DIR=/data/local/tmp/stackplz-kpm-test
KPM=$DIR/stackplz-kpm.kpm

echo "=== load default profile (no args) ==="
$KP kpm load "$KPM"
echo "load_exit=$?"
echo "=== list/info ==="
$KP kpm list || true
$KP kpm num || true
$KP kpm info stackplz-kpm 2>/dev/null || true
echo "=== status ==="
$KP kpm ctl0 stackplz-kpm "status" || true

/system/bin/yes >/dev/null 2>&1 &
PID=$!
sleep 0.3
echo "=== target pid=$PID ==="
tr '\0' ' ' < /proc/$PID/cmdline; echo
ps -p "$PID" | head -3

echo "=== bind ==="
$KP kpm ctl0 stackplz-kpm "bind pid=$PID mode=pid"
echo "bind_ctl_exit=$?"

i=0
while [ "$i" -lt 20 ]; do
  STATUS=$($KP kpm ctl0 stackplz-kpm "status" 2>/dev/null || true)
  echo "poll_$i $STATUS"
  echo "$STATUS" | grep -q "binding=bound" && break
  i=$((i + 1))
  sleep 0.2
done

echo "=== final status ==="
$KP kpm ctl0 stackplz-kpm "status" || true

$KP kpm ctl0 stackplz-kpm "clear" || true
kill -9 "$PID" 2>/dev/null || true
echo "=== after clear ==="
$KP kpm ctl0 stackplz-kpm "status" || true
echo VERIFY_DONE
