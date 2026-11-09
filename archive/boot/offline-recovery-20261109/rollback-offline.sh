#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
[ "$(id -u)" = 0 ]
stage=$(cd "$(dirname "$0")" && pwd)
source_config="$stage/config.rollback.private.plist"
sha() { shasum -a 256 "$1" | awk '{print $1}'; }
info() { diskutil info -plist "$1" | plutil -extract "$2" raw -o - -; }
efi_uuid=00000000-0000-0000-0000-000000000001
data_uuid=00000000-0000-0000-0000-000000000002
expected=e1c8fc66fd293746521941dab468b0a68a07dacd13b15389bbd2d69e6ff1634e
legacy=5b5a9f7aa270e1db24957d951bb198065824770f000cfcffb8183fd96c3fc92b
quiet=ead26ddfb4e249dab2d85fb53a7918d73da58977d0b33cb2e719fcfc45a51dac
[ "$(sha "$source_config")" = "$expected" ]
plutil -lint "$source_config"
efi_device=$(info "$efi_uuid" DeviceIdentifier)
[ "$(info "$efi_device" DiskUUID)" = "$efi_uuid" ]
data_mp=$(info "$data_uuid" MountPoint)
case "$data_mp" in /Volumes/*) ;; *) echo '拒绝非外接卷路径'; exit 1;; esac
[ "$(info "$data_uuid" Internal)" = false ]
physical_store=$(info "$data_uuid" APFSPhysicalStores.0.APFSPhysicalStore)
[ "$(info "$physical_store" ParentWholeDisk)" = "$(info "$efi_device" ParentWholeDisk)" ]
daily="$data_mp/Library/Application Support/WTLArrowTiming.EXAMPLE/trial-config.plist"
[ "$(sha "$daily")" = "$quiet" ]
esp_mp=$(info "$efi_device" MountPoint)
[ -n "$esp_mp" ]
[ "$(sha "$esp_mp/EFI/OC/config.plist")" = "$legacy" ]
[ "$(sha "$data_mp/Library/KernelCollections/AuxiliaryKernelExtensions.kc")" = 910da863fd31d575d792fdd3f2bb2bf59c295010fe2cc2a584e6ceffaf52fa27 ]
[ "$(sha "$data_mp/Library/GPUBundles/XeMTLDriver.bundle/Contents/MacOS/XeMTLDriver")" = 6f2e9c5b6306ee132ccca967e911a42d904c85f5f94917f6b65066fd007de696 ]
backup=$(mktemp -d "$stage/write-backup.XXXXXX")
chmod 700 "$backup"
cp "$daily" "$backup/daily.plist"
cp "$esp_mp/EFI/OC/config.plist" "$backup/efi.plist"
cmp "$daily" "$backup/daily.plist"
cmp "$esp_mp/EFI/OC/config.plist" "$backup/efi.plist"
diskutil unmount "$efi_device"
diskutil mount "$efi_device"
esp_mp=$(info "$efi_device" MountPoint)
efi_config="$esp_mp/EFI/OC/config.plist"
[ "$(info "$efi_device" DiskUUID)" = "$efi_uuid" ]
[ "$(sha "$efi_config")" = "$legacy" ]
[ "$(sha "$daily")" = "$quiet" ]
mutating=0
finish() {
 rc=$?
 trap - EXIT
 if [ "$rc" != 0 ] && [ "$mutating" = 1 ]; then
  set +e
  cp "$backup/efi.plist" "$efi_config"
  cp "$backup/daily.plist" "$daily"
  sync
  echo '发生错误，已尝试恢复两份原配置；请保留硬盘连接供核对。'
 fi
 exit "$rc"
}
trap finish EXIT
mutating=1
install -m 644 -o 0 -g 0 "$source_config" "$daily.wtl-rollback-new"
[ "$(sha "$daily.wtl-rollback-new")" = "$expected" ]
cp "$source_config" "$efi_config.wtl-rollback-new"
[ "$(sha "$efi_config.wtl-rollback-new")" = "$expected" ]
mv "$daily.wtl-rollback-new" "$daily"
mv "$efi_config.wtl-rollback-new" "$efi_config"
sync
[ "$(sha "$efi_config")" = "$expected" ]
[ "$(sha "$daily")" = "$expected" ]
plutil -lint "$efi_config" "$daily"
mutating=0
trap - EXIT
printf 'EFI=%s\nDAILY=%s\nRESTARTED=NO\n' "$expected" "$expected" > "$stage/rollback-result.txt"
diskutil unmount "$efi_device"
echo '回退写入完成。保留 3 秒菜单、温度和原驱动；未重启。'
