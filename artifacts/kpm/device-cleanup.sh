#!/system/bin/sh
KP=/data/adb/modules/KPatch-Next/bin/kpatch
echo "=== leftover pids ==="
ps -A | grep -E 'stackplz| /system/bin/yes|device-hidden' | grep -v grep || echo none
echo "=== clear/unload ==="
$KP kpm ctl0 stackplz-kpm "clear" || true
$KP kpm ctl0 stackplz-kpm "status" || true
$KP kpm unload stackplz-kpm
echo "unload_exit=$?"
$KP kpm list || true
$KP kpm num || true
echo CLEANUP_DONE
