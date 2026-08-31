#!/system/bin/sh
echo "=== module.prop ==="
cat /data/adb/modules/KPatch-Next/module.prop
echo "=== kpatch bin ==="
ls -la /data/adb/modules/KPatch-Next/bin
echo "=== patch dir ==="
ls -la /data/adb/modules/KPatch-Next/patch
echo "=== hello.kpm dmesg ==="
KP=/data/adb/modules/KPatch-Next/bin/kpatch
$KP kpm load /data/local/tmp/hello.kpm
echo "hello_load_exit=$?"
dmesg | grep -E "hello|unknown symbol|load_module" | tail -15
echo "=== try kthread-observer ==="
$KP kpm load /data/local/tmp/kthread-observer.kpm
echo "observer_load_exit=$?"
dmesg | grep -E "kthread-observer|unknown symbol" | tail -10
$KP kpm list
$KP kpm num
echo "=== stackplz kpm path without module ==="
cd /data/local/tmp/stackplz-kpm-test
./stackplz --pid 1 --task-source kpm --syscall getpid --nocheck -o /data/local/tmp/stackplz-kpm-test/kpm-missing.log
echo "stackplz_kpm_exit=$?"
echo "=== log ==="
cat /data/local/tmp/stackplz-kpm-test/kpm-missing.log 2>/dev/null | tail -30
