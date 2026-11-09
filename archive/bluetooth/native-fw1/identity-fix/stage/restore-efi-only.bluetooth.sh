#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
[ "$(id -u)" = 0 ]
root=$(cd "$(dirname "$0")" && pwd)
trial="$root/BluetoothTrial.2"
. "$root/esp.sh"
# BEGIN NORMALIZE
normalize_bluetooth_profile() {
    if cmp -s "$esp_config" "$trial/config.bluetooth.plist"; then
        cmp -s "$root/trial-config.plist" "$trial/config.d3.plist"
        write_config "$root/trial-config.plist"
    elif ! cmp -s "$esp_config" "$root/trial-config.plist"; then
        cmp -s "$esp_config" "$root/stable-config.plist"
    fi
}
# END NORMALIZE
mount_esp
trap unmount_esp EXIT
normalize_bluetooth_profile
unmount_esp
trap - EXIT
# Return the original helper to its original directory before executing it.
[ "$(shasum -a 256 "$trial/restore-efi-only.before.sh" | awk '{print $1}')" = 2fab6bafd51f2a87b1f30d95089691d2407a67c5c1d369ecf219ed67568d1db5 ]
cp "$trial/restore-efi-only.before.sh" "$root/restore-efi-only.sh.bt-original"
chown root:wheel "$root/restore-efi-only.sh.bt-original"
chmod 755 "$root/restore-efi-only.sh.bt-original"
mv -f "$root/restore-efi-only.sh.bt-original" "$root/restore-efi-only.sh"
exec /bin/bash "$root/restore-efi-only.sh"
