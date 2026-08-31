#!/system/bin/sh
echo "=== uptime ==="
cat /proc/uptime
uname -a
echo "=== last boot ==="
cat /proc/sys/kernel/random/boot_id
echo "=== kpm ==="
/data/adb/modules/KPatch-Next/bin/kpatch kpm num
echo "=== kmsg panic/oops ==="
dmesg | grep -iE "panic|Oops|stackplz|Internal error|die " | tail -20
echo "=== KP lines ==="
dmesg | grep "KP " | tail -20
