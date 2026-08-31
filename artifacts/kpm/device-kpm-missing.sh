#!/system/bin/sh
KP=/data/adb/modules/KPatch-Next/bin/kpatch
cd /data/local/tmp/stackplz-kpm-test

echo "=== unload diagnostic kthread-observer ==="
$KP kpm unload kthread-observer
echo "unload_exit=$?"
$KP kpm list
$KP kpm num

echo "=== stackplz --task-source=kpm without module ==="
./stackplz --pid 1 --task-source kpm --syscall getpid --nocheck -o kpm-missing.log
echo "stackplz_kpm_exit=$?"
echo "=== kpm-missing.log ==="
cat kpm-missing.log 2>/dev/null
