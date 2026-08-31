#!/system/bin/sh
set -eu

KP=/data/adb/modules/KPatch-Next/bin/kpatch
DIR=/data/local/tmp/stackplz-kpm-test
CTL="$KP kpm ctl0 stackplz-kpm"
EMPTY=$DIR/empty_proc_maps_smoke

cd "$DIR"
mkdir -p "$EMPTY"
chmod 0755 "$EMPTY"

cleanup() {
  "$KP" kpm ctl0 stackplz-kpm clear >/dev/null 2>&1 || true
  if [ -n "${PID:-}" ]; then
    chmod 0755 "$EMPTY" >/dev/null 2>&1 || true
    umount "/proc/$PID" >/dev/null 2>&1 || true
    kill -9 "$PID" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT INT TERM

"$KP" kpm ctl0 stackplz-kpm clear >/dev/null
./write_loop >/dev/null 2>&1 &
PID=$!
sleep 1
cp "/proc/$PID/maps" maps-visible.expected

"$KP" kpm ctl0 stackplz-kpm "bind pid=$PID mode=pid"
I=0
while [ "$I" -lt 50 ]; do
  STATUS=$("$KP" kpm ctl0 stackplz-kpm status)
  echo "$STATUS" | grep -q 'binding=bound' && break
  I=$((I + 1))
  sleep 0.1
done
echo "BIND_STATUS=$STATUS"
echo "$STATUS" | grep -q 'binding=bound'

mount -o bind "$EMPTY" "/proc/$PID"
chmod 000 "$EMPTY" 2>/dev/null || true
echo "HIDDEN_PS_LINES=$(ps -p "$PID" | wc -l)"
if [ -r "/proc/$PID/maps" ]; then echo HIDDEN_MAPS_READABLE=1; exit 1; else echo HIDDEN_MAPS_READABLE=0; fi

"$KP" kpm ctl0 stackplz-kpm maps
I=0
while [ "$I" -lt 100 ]; do
  STATUS=$("$KP" kpm ctl0 stackplz-kpm status)
  echo "$STATUS" | grep -q 'maps_state=ready' && break
  echo "$STATUS" | grep -q 'maps_state=error' && break
  I=$((I + 1))
  sleep 0.1
done
echo "MAPS_STATUS=$STATUS"
echo "$STATUS" | grep -q 'maps_state=ready'

SNAPSHOT=$(echo "$STATUS" | sed -n 's/.* maps_snapshot=\([0-9][0-9]*\).*/\1/p')
SIZE=$(echo "$STATUS" | sed -n 's/.* maps_size=\([0-9][0-9]*\).*/\1/p')
echo "MAPS_SNAPSHOT=$SNAPSHOT MAPS_SIZE=$SIZE EXPECTED_SIZE=$(wc -c < maps-visible.expected)"
rm -f maps-visible.actual
touch maps-visible.actual
OFFSET=0
while [ "$OFFSET" -lt "$SIZE" ]; do
  CHUNK=$("$KP" kpm ctl0 stackplz-kpm "maps-read snapshot=$SNAPSHOT offset=$OFFSET")
  HEX=$(echo "$CHUNK" | sed -n 's/.* data=\([0-9a-f]*\).*/\1/p')
  [ -n "$HEX" ]
  echo -n "$HEX" | xxd -r -p >> maps-visible.actual
  NEXT=$(wc -c < maps-visible.actual)
  [ "$NEXT" -gt "$OFFSET" ]
  OFFSET=$NEXT
done
[ "$OFFSET" -eq "$SIZE" ]
cmp maps-visible.expected maps-visible.actual
echo "MAPS_CMP=identical SHA256=$(sha256sum maps-visible.actual | awk '{print $1}')"
echo MAPS_SMOKE_DONE
