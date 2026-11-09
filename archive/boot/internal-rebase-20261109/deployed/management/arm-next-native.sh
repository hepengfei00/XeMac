#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
[ "$(id -u)" = 0 ]
[ "$(uname -m)" = x86_64 ]
[ "$(diskutil info -plist /System/Volumes/Data | plutil -extract VolumeUUID raw -o - -)" = 00000000-0000-0000-0000-000000000002 ]
if sysctl -n kern.bootargs | grep -q 'wtl-arrow-native=3'; then
 echo '当前已经是原生图形启动会话，请不要重复恢复一次性开关。'
 exit 1
fi
pkg=$(cd "$(dirname "$0")" && pwd)
root='/Library/Application Support/WTLArrowTiming.EXAMPLE'
hash() { shasum -a 256 "$1" | awk '{print $1}'; }
daily=$(hash "$root/trial-config.plist")
[ "$daily" = b68bdd2a54fce7b54103757ba37ab74e4cfc8d2eb20b69d53c01bf4fe1a8380c ] || [ "$daily" = 21730927c8c1422c9d6b6e4ed3ea4bece3d770b02d05ed893dc1bb784297dbec ]
[ "$(hash "$root/stable-config.plist")" = ec385e2c5194728766138ddf8c2a74c206299b61bc27ec5d3936ed76ed87abe6 ]
[ "$(hash "$root/gates.plist")" = 613bb51c7832b5c5c0c532cdb7c2bebb32e8eed9f6a52cb0352c886f1bd4c6fb ]
[ "$(hash "$root/gate-audit")" = c58d3322e99475eb0e35bf7bed6007710eb028cb586a9739a07852643c62fe20 ]
cd /
shasum -a 256 -c "$pkg/protected-artifacts.sha256" > "$pkg/arm-artifacts-check.log"
. "$root/esp.sh"
mount_esp
trap unmount_esp EXIT
current=$(hash "$esp_config")
[ "$current" = "$daily" ] || [ "$current" = ec385e2c5194728766138ddf8c2a74c206299b61bc27ec5d3936ed76ed87abe6 ]
[ "$(plutil -extract ACPI.Quirks.RebaseRegions raw -o - "$root/trial-config.plist")" = true ]
cd "$esp_mp/EFI/OC"
shasum -a 256 -c "$pkg/enabled-files.sha256" > "$pkg/arm-enabled-files-check.log"
if [ "$current" != "$daily" ]; then
 cp "$esp_config" "$pkg/efi-before-one-time-arm.private.plist"
 write_config "$root/trial-config.plist"
fi
if "$root/gate-audit" > "$pkg/gates-before-arm.txt" 2>&1; then
 echo '一次性启动开关已就绪，无需重写。'
else
 nvram -x -f "$root/gates.plist"
fi
"$root/gate-audit" > "$pkg/gates-after-arm.txt"
[ "$(hash "$esp_config")" = "$daily" ]
sync
echo 'READY_FOR_NEXT_NATIVE_BOOT_NO_RESTART'
echo '检查完成。现在可正常关机，拔掉救援U盘，再从原1TB盘启动。'
