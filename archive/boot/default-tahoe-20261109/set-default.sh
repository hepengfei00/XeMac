#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
[ "$(id -u)" = 0 ]
[ "$(sysctl -n kern.bootsessionuuid)" = 0649061D-6876-5AA6-913F-99873D2D00BC ]
stage=$(cd "$(dirname "$0")" && pwd)
var=4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:OCBtOrder
for pair in "OCBtOrder a72f2ca83dc117a00432039628b7c0382f5888a0532f6e9d080380e31fd86d98" "OCBt0000 7d8a86d4a80bea3e8137d732f979e5589a3a4b7e436e495a1edd9c2c27c123f0" "OCBt0080 3d9f936181fc9c5b1a135e334c89c1371bda6d3e59fa86a316b6a63553567bb3"; do
 read -r key expected <<< "$pair"
 nvram -x "4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:$key" > "$stage/$key.actual-before.xml"
 [ "$(shasum -a 256 "$stage/$key.actual-before.xml" | awk '{print $1}')" = "$expected" ]
done
changed=0
finish() {
 rc=$?
 trap - EXIT
 if [ "$rc" != 0 ] && [ "$changed" = 1 ]; then nvram -x -f "$stage/OCBtOrder.actual-before.xml"; fi
 exit "$rc"
}
trap finish EXIT
changed=1
nvram -x -f "$stage/desired.private.xml"
line=$(nvram "$var")
[ "${line#*$'\t'}" = '%80%00%00%00' ]
# Entries are kept byte-for-byte. No EFI, kernel cache or driver change.
for pair in "OCBt0000 7d8a86d4a80bea3e8137d732f979e5589a3a4b7e436e495a1edd9c2c27c123f0" "OCBt0080 3d9f936181fc9c5b1a135e334c89c1371bda6d3e59fa86a316b6a63553567bb3"; do
 read -r key expected <<< "$pair"
 [ "$(nvram -x "4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:$key" | shasum -a 256 | awk '{print $1}')" = "$expected" ]
done
nvram -x "$var" > "$stage/OCBtOrder.actual-after.xml"
chown hackintosh:staff "$stage"/*.actual-*.xml
chmod 600 "$stage"/*.actual-*.xml
printf 'DEFAULT_ORDER_TAHOE_0080_FIRST_WINDOWS_0000_RETAINED_NO_RESTART\n' | tee "$stage/RESULT"
changed=0
trap - EXIT
