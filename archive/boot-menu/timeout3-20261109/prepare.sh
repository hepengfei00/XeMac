#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
umask 077
[ "$(id -u)" = 0 ]
stage=$(cd "$(dirname "$0")" && pwd)
root='/Library/Application Support/WTLArrowTiming.EXAMPLE'
pkg="$root/BootMenu3s.1"
old=04eece81cf92b2c23e38e4e66cd2b44486df25ee18b6d02429fc408bfdc687b4
new=e1c8fc66fd293746521941dab468b0a68a07dacd13b15389bbd2d69e6ff1634e
hash() { shasum -a 256 "$1" | awk '{print $1}'; }
[ "$(sysctl -n kern.bootsessionuuid)" = 'E7B590BE-CE6B-5E92-B72C-BE812951B133' ]
[ "$(hash "$root/trial-config.plist")" = "$old" ]
[ "$(hash "$stage/config.timeout3.private.plist")" = "$new" ]
[ ! -e "$pkg" ]
bash "$root/health.sh"
mkdir "$pkg"
chmod 755 "$pkg"
cp "$stage/rollback.sh" "$pkg/rollback.sh"
chown root:wheel "$pkg/rollback.sh"
chmod 755 "$pkg/rollback.sh"
cp "$stage/config.timeout3.private.plist" "$pkg/config.timeout3.private.plist"
cp "$stage/ocvalidate" "$pkg/ocvalidate"
chmod 755 "$pkg/ocvalidate"
chmod 600 "$pkg/config.timeout3.private.plist"
"$pkg/ocvalidate" "$pkg/config.timeout3.private.plist"
[ "$(plutil -extract Misc.Boot.Timeout raw -o - "$pkg/config.timeout3.private.plist")" = 3 ]
[ "$(plutil -extract Misc.Boot.ShowPicker raw -o - "$pkg/config.timeout3.private.plist")" = true ]
. "$root/esp.sh"
completed=0
changed=0
cleanup() {
    rc=$?
    trap - EXIT
    set +e
    if [ "$completed" = 0 ]; then
        if [ "$changed" = 1 ]; then
            bash "$pkg/rollback.sh"
            echo "BOOT_MENU_ROLLBACK_EXIT=$?"
        fi
        unmount_esp
        touch "$pkg/INSTALL_FAILED"
    fi
    exit "$rc"
}
trap cleanup EXIT
mount_esp
[ "$(hash "$esp_config")" = "$old" ]
cmp -s "$esp_config" "$root/trial-config.plist"
[ ! -e "$esp_config.arrow-new" ]
[ ! -e "$root/trial-config.plist.menu-new" ]
mkdir "$pkg/backup"
chmod 700 "$pkg/backup"
cp -p "$esp_config" "$pkg/backup/efi-config.plist"
cp -p "$root/trial-config.plist" "$pkg/backup/trial-config.plist"
[ "$(hash "$pkg/backup/efi-config.plist")" = "$old" ]
# Include the temperature plugin/collector and existing boot helpers in the
# protected set. This task changes only the timeout in the two active profiles.
while IFS= read -r -d '' file; do
    [ "$file" = "$esp_config" ] || shasum -a 256 "$file"
done < <(find "$esp_mp/EFI" -type f -print0) > "$pkg/efi-files-before.sha256"
shasum -a 256 "$root/esp.sh" "$root/onboot-stable.sh" "$root/onboot-verified.sh" "$root/commit-next-boot.sh" "$root/restore-efi-only.sh" "$root/CPUTemperature.1/collect-nextboot.sh" /Library/LaunchDaemons/org.local.wtl-cpu-temperature-check.plist /Library/KernelCollections/AuxiliaryKernelExtensions.kc /Library/GPUBundles/XeMTLDriver.bundle/Contents/MacOS/XeMTLDriver > "$pkg/protected-before.sha256"
bless --getBoot > "$pkg/default-volume-before.txt"
[ "$(tail -n 1 "$pkg/default-volume-before.txt")" = /dev/disk1s4 ]
[ "$(diskutil info / | awk -F': *' '/Volume Name:/ {print $2; exit}')" = Tahoe ]
changed=1
cp -p "$root/trial-config.plist" "$root/trial-config.plist.menu-new"
cat "$pkg/config.timeout3.private.plist" > "$root/trial-config.plist.menu-new"
cmp -s "$root/trial-config.plist.menu-new" "$pkg/config.timeout3.private.plist"
mv -f "$root/trial-config.plist.menu-new" "$root/trial-config.plist"
write_config "$pkg/config.timeout3.private.plist"
[ "$(hash "$root/trial-config.plist")" = "$new" ]
[ "$(hash "$esp_config")" = "$new" ]
shasum -a 256 -c "$pkg/efi-files-before.sha256" > "$pkg/efi-files-after-check.txt"
shasum -a 256 -c "$pkg/protected-before.sha256"
bless --getBoot > "$pkg/default-volume-after.txt"
cmp -s "$pkg/default-volume-before.txt" "$pkg/default-volume-after.txt"
bash "$root/health.sh"
shasum -a 256 "$esp_config" "$root/trial-config.plist" > "$pkg/installed.sha256"
chmod 644 "$pkg/installed.sha256"
sysctl -n kern.bootsessionuuid > "$pkg/prepared-boot"
chmod 644 "$pkg/prepared-boot"
unmount_esp
sync
touch "$pkg/TAHOE_AUTOBOOT_3_SECONDS_INSTALLED_NO_RESTART"
chmod 644 "$pkg/TAHOE_AUTOBOOT_3_SECONDS_INSTALLED_NO_RESTART"
completed=1
trap - EXIT
echo TAHOE_AUTOBOOT_3_SECONDS_INSTALLED_NO_RESTART
