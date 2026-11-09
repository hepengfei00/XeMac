#!/bin/bash
set -euo pipefail
umask 077
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
[ "$(id -u)" = 0 ]
stage=$(cd "$(dirname "$0")" && pwd)
uuid=8077D2F7-BD2B-56F5-9A88-059C8D94CB0D
info() { diskutil info -plist "$1" | plutil -extract "$2" raw -o - -; }
sha() { shasum -a 256 "$1" | awk '{print $1}'; }
case "${1:-on}" in
 on)
  source_config="$stage/config.rebase.private.plist"
  old=0e3bc92a51d7c76dc9c7bc73259da730c699948e036318cc8cfdf1991fdaa59b
  new=923b800c43e75d2ee72e085b3b2c952f98c8c338e53a0aec7224107d676b01e0
  ;;
 off)
  source_config="$stage/usb-before/EFI/OC/config.plist"
  old=923b800c43e75d2ee72e085b3b2c952f98c8c338e53a0aec7224107d676b01e0
  new=0e3bc92a51d7c76dc9c7bc73259da730c699948e036318cc8cfdf1991fdaa59b
  ;;
 *) exit 1;;
esac
[ "$(sha "$source_config")" = "$new" ]
plutil -lint "$source_config"
dev=$(info "$uuid" DeviceIdentifier)
[ "$(info "$dev" DiskUUID)" = "$uuid" ]
[ "$(info "$dev" Internal)" = false ]
[ "$(info "$dev" BusProtocol)" = USB ]
[ "$(info "$(info "$dev" ParentWholeDisk)" TotalSize)" = EXAMPLE_RESCUE_DISK_BYTES ]
mp=$(info "$dev" MountPoint)
[ -n "$mp" ]
[ "$(sha "$mp/EFI/OC/config.plist")" = "$old" ]
diskutil unmount "$dev"
diskutil mount "$dev"
mp=$(info "$dev" MountPoint)
config="$mp/EFI/OC/config.plist"
[ "$(sha "$config")" = "$old" ]
cd "$mp/EFI"
shasum -a 256 -c "$stage/efi-other-files.sha256" > "$stage/rebase-other-files-before.log"
cd "$stage"
backup=$(mktemp "$stage/rebase-before.XXXXXX")
cp "$config" "$backup"
chown "$SUDO_UID:$SUDO_GID" "$backup"
cmp "$config" "$backup"
mutating=0
report_moved=0
cleanup() {
 rc=$?
 trap - EXIT
 if [ "$rc" != 0 ] && [ "$mutating" = 1 ]; then
  set +e
  cp "$backup" "$config"
  if [ "$report_moved" = 1 ] && [ ! -e "$mp/SysReport" ]; then
   mv "$mp/SysReport-before-rebase-20261109" "$mp/SysReport"
  fi
  sync
  echo '写入失败，已尝试恢复原配置；请保留U盘连接供核对。'
 fi
 exit "$rc"
}
trap cleanup EXIT
mutating=1
if [ "${1:-on}" = on ] && [ -d "$mp/SysReport" ]; then
 [ ! -e "$mp/SysReport-before-rebase-20261109" ]
 mv "$mp/SysReport" "$mp/SysReport-before-rebase-20261109"
 report_moved=1
fi
cp "$source_config" "$config.rebase-new"
[ "$(sha "$config.rebase-new")" = "$new" ]
mv "$config.rebase-new" "$config"
sync
[ "$(sha "$config")" = "$new" ]
plutil -lint "$config"
cd "$mp/EFI"
shasum -a 256 -c "$stage/efi-other-files.sha256" > "$stage/rebase-other-files-after.log"
cd "$stage"
printf 'MODE=%s\nCONFIG=%s\nRESTARTED=NO\n' "${1:-on}" "$new" > "$stage/rebase-result.txt"
chown "$SUDO_UID:$SUDO_GID" "$stage/rebase-result.txt"
mutating=0
trap - EXIT
diskutil unmount "$dev"
diskutil mount readOnly "$dev"
echo 'Setup complete。救援U盘地址重定位设置已写入，等待独立核对；未重启。'
