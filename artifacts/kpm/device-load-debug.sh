#!/system/bin/sh
KP=/data/adb/modules/KPatch-Next/bin/kpatch
KPM=/data/local/tmp/stackplz-kpm-test/stackplz-kpm.kpm
PROFILE=oneplus-plk110-a16-b4999618-d05

echo "=== file ==="
ls -la "$KPM"
echo "=== hello ==="
$KP hello
echo "hello_exit=$?"
echo "=== try load exact, capture stderr ==="
$KP kpm load "$KPM" "profile=$PROFILE" > /data/local/tmp/stackplz-kpm-test/load.out 2> /data/local/tmp/stackplz-kpm-test/load.err
echo "load_exit=$?"
echo "=== stdout ==="
cat /data/local/tmp/stackplz-kpm-test/load.out
echo "=== stderr ==="
cat /data/local/tmp/stackplz-kpm-test/load.err
echo "=== dmesg tail ==="
dmesg 2>/dev/null | grep -i -E "kpatch|kpm|stackplz|kpmodule" | tail -40
echo "=== logcat kernel ==="
logcat -d -b kernel -t 80 2>/dev/null | grep -i -E "kpatch|kpm|stackplz|symbol" | tail -40
echo "=== try hello.kpm load ==="
$KP kpm load /data/local/tmp/hello.kpm > /data/local/tmp/stackplz-kpm-test/hello.out 2> /data/local/tmp/stackplz-kpm-test/hello.err
echo "hello_kpm_exit=$?"
cat /data/local/tmp/stackplz-kpm-test/hello.out
cat /data/local/tmp/stackplz-kpm-test/hello.err
$KP kpm list
$KP kpm num
