#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
[ "$(id -u)" = 0 ]
pkg=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$pkg/.." && pwd)
old=04eece81cf92b2c23e38e4e66cd2b44486df25ee18b6d02429fc408bfdc687b4
new=e1c8fc66fd293746521941dab468b0a68a07dacd13b15389bbd2d69e6ff1634e
hash() { shasum -a 256 "$1" | awk '{print $1}'; }
current=$(hash "$root/trial-config.plist")
[ "$current" = "$old" ] || [ "$current" = "$new" ]
[ "$(hash "$pkg/backup/trial-config.plist")" = "$old" ]
. "$root/esp.sh"
mount_esp
trap unmount_esp EXIT
current=$(hash "$esp_config")
[ "$current" = "$old" ] || [ "$current" = "$new" ]
rm -f "$root/trial-config.plist.menu-new" "$esp_config.arrow-new"
cp -p "$pkg/backup/trial-config.plist" "$root/trial-config.plist.menu-restore"
mv -f "$root/trial-config.plist.menu-restore" "$root/trial-config.plist"
write_config "$pkg/backup/efi-config.plist"
cmp -s "$pkg/backup/trial-config.plist" "$root/trial-config.plist"
unmount_esp
trap - EXIT
echo BOOT_MENU_TIMEOUT_ROLLED_BACK_SENSOR_RETAINED_NO_RESTART
