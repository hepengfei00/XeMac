#!/bin/bash
set -euo pipefail
umask 077
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
[ "$(id -u)" = 0 ]
stage=$(cd "$(dirname "$0")" && pwd)
uuid=8077D2F7-BD2B-56F5-9A88-059C8D94CB0D
dev=$(diskutil info -plist "$uuid" | plutil -extract DeviceIdentifier raw -o - -)
[ "$(diskutil info -plist "$dev" | plutil -extract DiskUUID raw -o - -)" = "$uuid" ]
diskutil mount readOnly "$dev"
mp=$(diskutil info -plist "$dev" | plutil -extract MountPoint raw -o - -)
[ -n "$mp" ]
dest=$(mktemp -d "$stage/current-report.XXXXXX")
ditto --norsrc "$mp/EFI" "$dest/EFI"
if [ -d "$mp/SysReport" ]; then
 ditto --norsrc "$mp/SysReport" "$dest/SysReport"
 echo 'FRESH_REPORT_PRESENT=YES'
else
 echo 'FRESH_REPORT_PRESENT=NO'
fi
mkdir "$dest/OpenCoreLogs"
shopt -s nullglob
for f in "$mp"/opencore-*.txt; do cp -p "$f" "$dest/OpenCoreLogs/"; done
diskutil info -plist "$dev" > "$dest/efi-info.private.plist"
chmod 700 "$dest"
chown -R "$SUDO_UID:$SUDO_GID" "$dest"
printf '%s\n' "$dest" > "$stage/current-report-path.txt"
echo 'Collection complete。新报告已读取；U盘保持只读挂载，等待核对。'
