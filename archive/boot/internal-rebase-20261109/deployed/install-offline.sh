#!/bin/bash
set -euo pipefail
umask 077
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
[ "$(id -u)" = 0 ]
stage=$(cd "$(dirname "$0")" && pwd)
info() { diskutil info -plist "$1" | plutil -extract "$2" raw -o - -; }
hash() { shasum -a 256 "$1" | awk '{print $1}'; }
esp_uuid=00000000-0000-0000-0000-000000000001
data_uuid=00000000-0000-0000-0000-000000000002
old_native=e1c8fc66fd293746521941dab468b0a68a07dacd13b15389bbd2d69e6ff1634e
new_native=b68bdd2a54fce7b54103757ba37ab74e4cfc8d2eb20b69d53c01bf4fe1a8380c
old_fallback=5b5a9f7aa270e1db24957d951bb198065824770f000cfcffb8183fd96c3fc92b
new_fallback=ec385e2c5194728766138ddf8c2a74c206299b61bc27ec5d3936ed76ed87abe6
esp_dev=$(info "$esp_uuid" DeviceIdentifier)
[ "$(info "$esp_dev" DiskUUID)" = "$esp_uuid" ]
[ "$(info "$esp_dev" Internal)" = false ]
[ "$(info "$esp_dev" BusProtocol)" = USB ]
data_mp=$(info "$data_uuid" MountPoint)
case "$data_mp" in /Volumes/*) ;; *) exit 1;; esac
store=$(info "$data_uuid" APFSPhysicalStores.0.APFSPhysicalStore)
[ "$(info "$store" ParentWholeDisk)" = "$(info "$esp_dev" ParentWholeDisk)" ]
[ "$(info "$(info "$esp_dev" ParentWholeDisk)" TotalSize)" = EXAMPLE_SYSTEM_DISK_BYTES ]
root="$data_mp/Library/Application Support/WTLArrowTiming.EXAMPLE"
pkg="$data_mp/Library/Application Support/WTLStable/WTL-265K-Rebase-20261109-v2"
[ ! -e "$pkg" ]
[ ! -e "$stage/current-efi-before" ]
desktop="$data_mp/Users/hackintosh/Desktop"
[ -d "$desktop" ]
[ ! -e "$desktop/准备原盘下次启动.command" ]
cd "$stage/payload"
shasum -a 256 -c "$stage/payload.sha256" > "$stage/payload-check.log"
cd "$root"
shasum -a 256 -c "$stage/root-all-before.sha256" > "$stage/root-before-check.log"
cd "$data_mp"
shasum -a 256 -c "$stage/protected-artifacts.sha256" > "$stage/artifacts-before-check.log"
cd "$stage"
[ "$(hash "$root/trial-config.plist")" = "$old_native" ]
[ "$(hash "$root/stable-config.plist")" = "$old_fallback" ]
[ "$(hash "$stage/payload/config.debug.private.plist")" = "$new_native" ]
[ "$(hash "$stage/payload/config.fallback.private.plist")" = "$new_fallback" ]
[ "$(hash "$stage/restore.preserve-earlyclaim.sh")" = 20d1063c6bf1fe50b7afb69956023b755fc0718022aefc448a170253c03d25fe ]
diskutil mount readOnly "$esp_dev"
esp_mp=$(info "$esp_dev" MountPoint)
config="$esp_mp/EFI/OC/config.plist"
[ "$(hash "$config")" = "$old_native" ]
cd "$esp_mp/EFI/OC"
shasum -a 256 -c "$stage/enabled-files.sha256" > "$stage/enabled-before-check.log"
cd "$stage"
ditto --norsrc "$esp_mp/EFI" "$stage/current-efi-before"
chmod 700 "$stage/current-efi-before"
chown -R "$SUDO_UID:$SUDO_GID" "$stage/current-efi-before"
cd "$esp_mp/EFI"
find . -type f ! -path './OC/config.plist' -exec shasum -a 256 '{}' + > "$stage/efi-other-before.sha256"
cd "$stage/current-efi-before"
shasum -a 256 -c "$stage/efi-other-before.sha256" > "$stage/efi-backup-check.log"
cmp -s "$config" "$stage/current-efi-before/OC/config.plist"
cd "$stage"
mkdir -p "$pkg/backup/root"
chmod 700 "$pkg" "$pkg/backup" "$pkg/backup/root"
ditto --norsrc "$stage/current-efi-before" "$pkg/backup/EFI-before"
cd "$pkg/backup/EFI-before"
shasum -a 256 -c "$stage/efi-other-before.sha256" > "$stage/efi-package-backup-check.log"
cmp -s "$config" "$pkg/backup/EFI-before/OC/config.plist"
cd "$stage"
for name in trial-config.plist stable-config.plist restore.sh; do
 cp -p "$root/$name" "$pkg/backup/root/$name"
 cmp -s "$root/$name" "$pkg/backup/root/$name"
done
for name in config.debug.private.plist config.quiet.private.plist config.fallback.private.plist toggle-debug.sh README.md ocvalidate arm-next-native.sh 准备原盘下次启动.command; do
 install -m 600 -o 0 -g 0 "$stage/payload/$name" "$pkg/$name"
done
chmod 755 "$pkg/toggle-debug.sh" "$pkg/ocvalidate"
cp "$stage/protected-artifacts.sha256" "$pkg/protected-artifacts.sha256"
cp "$stage/enabled-files.sha256" "$pkg/enabled-files.sha256"
diskutil unmount "$esp_dev"
diskutil mount "$esp_dev"
esp_mp=$(info "$esp_dev" MountPoint)
config="$esp_mp/EFI/OC/config.plist"
[ "$(hash "$config")" = "$old_native" ]
mutating=0
report_moved=0
cleanup() {
 rc=$?
 trap - EXIT
 if [ "$rc" != 0 ] && [ "$mutating" = 1 ]; then
  set +e
  for name in trial-config.plist stable-config.plist restore.sh; do
   cp -p "$pkg/backup/root/$name" "$root/$name"
  done
  cp "$pkg/backup/EFI-before/OC/config.plist" "$config"
  if [ "$report_moved" = 1 ] && [ ! -e "$esp_mp/SysReport" ]; then
   mv "$esp_mp/SysReport-before-internal-rebase-20261109" "$esp_mp/SysReport"
  fi
  sync
  echo 'INSTALL_FAILED_ORIGINAL_FILES_RESTORE_ATTEMPTED_NO_RESTART' > "$pkg/INSTALL_FAILED"
 fi
 exit "$rc"
}
trap cleanup EXIT
mutating=1
for pair in 'config.fallback.private.plist:stable-config.plist' 'config.debug.private.plist:trial-config.plist'; do
 src=${pair%%:*}; dst=${pair#*:}
 install -m 644 -o 0 -g 0 "$pkg/$src" "$root/$dst.rebase-new"
 cmp -s "$pkg/$src" "$root/$dst.rebase-new"
 mv "$root/$dst.rebase-new" "$root/$dst"
done
install -m 755 -o 0 -g 0 "$stage/restore.preserve-earlyclaim.sh" "$root/restore.sh.rebase-new"
mv "$root/restore.sh.rebase-new" "$root/restore.sh"
if [ -d "$esp_mp/SysReport" ]; then
 [ ! -e "$esp_mp/SysReport-before-internal-rebase-20261109" ]
 mv "$esp_mp/SysReport" "$esp_mp/SysReport-before-internal-rebase-20261109"
 report_moved=1
fi
cp "$pkg/config.debug.private.plist" "$config.rebase-new"
cmp -s "$pkg/config.debug.private.plist" "$config.rebase-new"
mv "$config.rebase-new" "$config"
sync
[ "$(hash "$config")" = "$new_native" ]
[ "$(hash "$root/trial-config.plist")" = "$new_native" ]
[ "$(hash "$root/stable-config.plist")" = "$new_fallback" ]
for f in "$config" "$root/trial-config.plist" "$root/stable-config.plist" "$pkg/config.debug.private.plist" "$pkg/config.quiet.private.plist" "$pkg/config.fallback.private.plist"; do
 plutil -lint "$f"
 [ "$(plutil -extract ACPI.Quirks.RebaseRegions raw -o - "$f")" = true ]
done
[ "$(plutil -extract Misc.Boot.Timeout raw -o - "$config")" = 3 ]
cd "$esp_mp/EFI"
shasum -a 256 -c "$stage/efi-other-before.sha256" > "$stage/efi-other-after-check.log"
cd "$esp_mp/EFI/OC"
shasum -a 256 -c "$stage/enabled-files.sha256" > "$stage/enabled-after-check.log"
cd "$root"
shasum -a 256 -c "$stage/root-unchanged-after.sha256" > "$stage/root-after-check.log"
cd "$data_mp"
shasum -a 256 -c "$stage/protected-artifacts.sha256" > "$stage/artifacts-after-check.log"
cd "$stage"
printf 'EFI_DAILY=%s\nACTIVE_FALLBACK=%s\nRESTARTED=NO\n' "$new_native" "$new_fallback" > "$stage/result.txt"
chown "$SUDO_UID:$SUDO_GID" "$stage/result.txt"
cp "$stage/result.txt" "$pkg/applied.txt"
touch "$pkg/INSTALLED_NEXT_NATIVE_BOOT_PENDING"
install -m 755 -o 0 -g 0 "$pkg/准备原盘下次启动.command" "$desktop/准备原盘下次启动.command"
mutating=0
trap - EXIT
diskutil unmount "$esp_dev"
diskutil mount readOnly "$esp_dev"
echo 'Setup complete。原盘动态重定位、活动回退和日志开关已同步；3秒、温度及驱动保留。未重启。'
