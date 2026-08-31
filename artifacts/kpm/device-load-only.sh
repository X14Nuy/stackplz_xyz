#!/system/bin/sh
KP=/data/adb/modules/KPatch-Next/bin/kpatch
KPM=/data/local/tmp/stackplz-kpm-test/stackplz-kpm.kpm
PROFILE=oneplus-plk110-a16-b4999618-d05
echo "=== nm-like size ==="
ls -l "$KPM"
echo "=== load ==="
$KP kpm load "$KPM" "profile=$PROFILE"
echo "load_exit=$?"
echo "=== dmesg ==="
dmesg | tail -40
echo "=== list/num ==="
$KP kpm list
$KP kpm num
