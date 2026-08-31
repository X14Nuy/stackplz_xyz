#!/system/bin/sh
KP=/data/adb/modules/KPatch-Next/bin/kpatch
KPM=/data/local/tmp/stackplz-kpm-test/stackplz-kpm.kpm
PROFILE=oneplus-plk110-a16-b4999618-d05
echo "=== uptime / kpm num before ==="
uptime
$KP kpm num || true
$KP kpm list || true
echo "=== load diagnostic ==="
$KP kpm load "$KPM" "profile=$PROFILE"
echo "load_exit=$?"
echo "=== dmesg stackplz ==="
dmesg | grep -E "stackplz-kpm|unknown symbol|unsupported RELA|overflow in relocation|load_module" | tail -30
echo "=== list/info after ==="
$KP kpm list || true
$KP kpm num || true
$KP kpm info stackplz-kpm 2>/dev/null || true
echo "=== uptime after ==="
uptime
echo DIAG_DONE
