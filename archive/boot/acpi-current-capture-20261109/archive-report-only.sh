#!/bin/bash
set -euo pipefail
umask 077
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
[ "$(id -u)" = 0 ]
stage=$(cd "$(dirname "$0")" && pwd)
uuid=8077D2F7-BD2B-56F5-9A88-059C8D94CB0D
info() { diskutil info -plist "$1" | plutil -extract "$2" raw -o - -; }
sha() { shasum -a 256 "$1" | awk '{print $1}'; }
dev=$(info "$uuid" DeviceIdentifier)
[ "$(info "$dev" DiskUUID)" = "$uuid" ]
[ "$(info "$dev" Internal)" = false ]
[ "$(info "$dev" BusProtocol)" = USB ]
[ "$(info "$(info "$dev" ParentWholeDisk)" TotalSize)" = EXAMPLE_RESCUE_DISK_BYTES ]
cleanup() { diskutil unmount "$dev" >/dev/null 2>&1 || true; }
trap cleanup EXIT
diskutil mount "$dev"
mp=$(info "$dev" MountPoint)
[ -n "$mp" ]
expected=0e3bc92a51d7c76dc9c7bc73259da730c699948e036318cc8cfdf1991fdaa59b
[ "$(sha "$mp/EFI/OC/config.plist")" = "$expected" ]
[ "$(plutil -extract Misc.Debug.SysReport raw -o - "$mp/EFI/OC/config.plist")" = true ]
[ "$(plutil -extract Misc.Boot.ShowPicker raw -o - "$mp/EFI/OC/config.plist")" = true ]
[ "$(plutil -extract Misc.Boot.Timeout raw -o - "$mp/EFI/OC/config.plist")" = 0 ]
[ "$(df -Pk "$mp" | awk 'NR==2 {print $4}')" -gt 10240 ]
cmp "$mp/EFI/OC/config.plist" "$stage/usb-before/EFI/OC/config.plist"
cd "$mp/EFI"
shasum -a 256 -c "$stage/efi-files.sha256" > "$stage/efi-verify-before.log"
cd "$mp/SysReport"
shasum -a 256 -c "$stage/report-files.sha256" > "$stage/report-verify-before.log"
cd "$stage"
[ ! -e "$mp/SysReport-before-capture-20261109" ]
mv "$mp/SysReport" "$mp/SysReport-before-capture-20261109"
sync
[ "$(sha "$mp/EFI/OC/config.plist")" = "$expected" ]
printf 'CONFIG_UNCHANGED=%s\nOLD_REPORT_ARCHIVED=YES\nRESTARTED=NO\n' "$expected" > "$stage/result.txt"
diskutil unmount "$dev"
diskutil mount readOnly "$dev"
trap - EXIT
echo 'Setup complete。仅归档旧报告，所有启动配置和驱动字节保持一致。'
