#!/system/bin/sh
echo "=== leftover kpm ==="
/data/adb/modules/KPatch-Next/bin/kpatch kpm list
/data/adb/modules/KPatch-Next/bin/kpatch kpm num
echo "=== leftover stackplz/sleep ==="
ps -A 2>/dev/null | grep -E "stackplz|sleep" | grep -v grep | head -20
echo "=== leftover mounts ==="
mount | grep stackplz-kpm-test || echo none
echo "=== proc 30700 ==="
ls -ld /proc/30700 2>&1 | head -1
echo "=== empty_proc mode ==="
ls -ld /data/local/tmp/stackplz-kpm-test/empty_proc
echo "=== test dir ==="
ls -la /data/local/tmp/stackplz-kpm-test | head -40
