#!/system/bin/sh
set -u
KP=/data/adb/modules/KPatch-Next/bin/kpatch
KPM=/data/local/tmp/stackplz-kpm-test/stackplz-kpm.kpm
PROFILE=oneplus-plk110-a16-b4999618-d05

echo "=== D02 wrong profile load ==="
$KP kpm load "$KPM" "profile=this-profile-does-not-exist"
echo "load_exit=$?"
echo "=== list after mismatch ==="
$KP kpm list
echo "=== num after mismatch ==="
$KP kpm num

echo "=== D01 exact profile load ==="
$KP kpm load "$KPM" "profile=$PROFILE"
echo "load_exit=$?"
echo "=== list after exact load ==="
$KP kpm list
echo "=== info ==="
$KP kpm info stackplz-kpm
echo "=== status ==="
$KP kpm ctl0 stackplz-kpm "status"
echo "=== profile ==="
$KP kpm ctl0 stackplz-kpm "profile"
