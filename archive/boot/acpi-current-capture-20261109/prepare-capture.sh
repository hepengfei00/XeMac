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
parent=$(info "$dev" ParentWholeDisk)
[ "$(info "$parent" TotalSize)" = EXAMPLE_RESCUE_DISK_BYTES ]
cleanup() { diskutil unmount "$dev" >/dev/null 2>&1 || true; }
trap cleanup EXIT
diskutil mount readOnly "$dev"
mp=$(info "$dev" MountPoint)
[ -n "$mp" ]
[ ! -e "$stage/usb-before" ]
ditto --norsrc "$mp" "$stage/usb-before"
chmod 700 "$stage/usb-before"
chown -R "$SUDO_UID:$SUDO_GID" "$stage/usb-before"
printf 'SNAPSHOT_READY\n' > "$stage/inspect.ready"
echo '现场副本已保存，正在等待本地核对；请保持窗口和U盘连接。'
ready=0
for ((i=0;i<900;i++)); do
 if [ -f "$stage/candidate.ready" ]; then ready=1; break; fi
 if [ -f "$stage/STOP" ]; then echo '已停止，仅保存现场副本。'; exit 1; fi
 sleep 1
done
[ "$ready" = 1 ]
rel=$(cat "$stage/target-relative.txt")
[ "$rel" = EFI/OC/config.plist ]
before_sha=$(cat "$stage/source.sha256")
after_sha=$(cat "$stage/candidate.sha256")
[[ "$before_sha" =~ ^[a-f0-9]{64}$ ]]
[[ "$after_sha" =~ ^[a-f0-9]{64}$ ]]
[ "$(sha "$mp/$rel")" = "$before_sha" ]
[ "$(sha "$stage/candidate.private.plist")" = "$after_sha" ]
plutil -lint "$stage/candidate.private.plist"
diskutil unmount "$dev"
diskutil mount "$dev"
mp=$(info "$dev" MountPoint)
[ "$(info "$dev" DiskUUID)" = "$uuid" ]
[ "$(sha "$mp/$rel")" = "$before_sha" ]
if [ -e "$mp/SysReport" ]; then
 archive=SysReport-before-capture-20261109
 [ ! -e "$mp/$archive" ]
 mv "$mp/SysReport" "$mp/$archive"
fi
cp "$stage/candidate.private.plist" "$mp/$rel.capture-new"
[ "$(sha "$mp/$rel.capture-new")" = "$after_sha" ]
mv "$mp/$rel.capture-new" "$mp/$rel"
sync
[ "$(sha "$mp/$rel")" = "$after_sha" ]
plutil -lint "$mp/$rel"
printf 'CONFIG=%s\nRESTARTED=NO\n' "$after_sha" > "$stage/result.txt"
diskutil unmount "$dev"
diskutil mount readOnly "$dev"
trap - EXIT
echo 'Setup complete。U盘已重新只读挂载，等待独立回读；暂时不要拔盘。'
