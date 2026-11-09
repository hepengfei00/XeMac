#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
[ "$(id -u)" = 0 ]
pkg=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$pkg/.." && pwd)
expected_old=c0cf8993dc4926da6031e03699529e02c00a1aa5ae2842aa413d6f5d3162bf68
expected_new=04eece81cf92b2c23e38e4e66cd2b44486df25ee18b6d02429fc408bfdc687b4
hash() { shasum -a 256 "$1" | awk '{print $1}'; }
current=$(hash "$root/trial-config.plist")
[ "$current" = "$expected_old" ] || [ "$current" = "$expected_new" ]
[ "$(hash "$pkg/backup/trial-config.plist")" = "$expected_old" ]
. "$root/esp.sh"
mount_esp
trap unmount_esp EXIT
current=$(hash "$esp_config")
if [ "$current" != "$expected_old" ] && [ "$current" != "$expected_new" ]; then
    cmp -s "$esp_config" "$root/stable-config.plist"
fi
touch "$pkg/COLLECTION_DISABLED"
if [ -f /Library/LaunchDaemons/org.local.wtl-cpu-temperature-check.plist ]; then
    cmp -s /Library/LaunchDaemons/org.local.wtl-cpu-temperature-check.plist "$pkg/collector-job.plist"
    launchctl bootout system/org.local.wtl-cpu-temperature-check 2>/dev/null || true
    rm /Library/LaunchDaemons/org.local.wtl-cpu-temperature-check.plist
fi
# Restore known pre-install EFI files, including EarlyClaim if the existing
# graphics recovery path removed it. Do not invoke the legacy restore.sh.
ditto --norsrc "$pkg/backup/EFI-before" "$esp_mp/EFI"
if [ -d "$esp_mp/EFI/OC/Kexts/SMCProcessor.kext.cpu-temperature-new" ]; then
    rm -r "$esp_mp/EFI/OC/Kexts/SMCProcessor.kext.cpu-temperature-new"
fi
rm -f "$esp_config.arrow-new" "$root/trial-config.plist.cpu-temperature-new"
sensor="$esp_mp/EFI/OC/Kexts/SMCProcessor.kext"
if [ -d "$sensor" ]; then
    cmp -s "$sensor/Contents/MacOS/SMCProcessor" "$pkg/SMCProcessor.kext/Contents/MacOS/SMCProcessor"
    cmp -s "$sensor/Contents/Info.plist" "$pkg/SMCProcessor.kext/Contents/Info.plist"
    rm -r "$sensor"
fi
diff -qr "$pkg/backup/EFI-before" "$esp_mp/EFI"
cp -p "$pkg/backup/trial-config.plist" "$root/trial-config.plist.cpu-temperature-restore"
mv -f "$root/trial-config.plist.cpu-temperature-restore" "$root/trial-config.plist"
cmp -s "$pkg/backup/trial-config.plist" "$root/trial-config.plist"
unmount_esp
trap - EXIT
touch "$pkg/ROLLED_BACK"
echo CPU_TEMPERATURE_CONFIGURATION_ROLLED_BACK_NO_RESTART
