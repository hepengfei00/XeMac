#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
[ "$(id -u)" = 0 ]
root='/Library/Application Support/WTLArrowTiming.EXAMPLE'
trial="$root/BluetoothTrial.2"
cmp "$trial/config.d3.plist" "$root/trial-config.plist"
[ "$(shasum -a 256 "$root/Arrow-Native-candidate.kc" | awk '{print $1}')" = 910da863fd31d575d792fdd3f2bb2bf59c295010fe2cc2a584e6ceffaf52fa27 ]
. "$root/esp.sh"
mount_esp
trap unmount_esp EXIT
early="$esp_mp/EFI/OC/Kexts/WTLArrowTrial3/WTLArrowEarlyClaim.kext"
if [ ! -s "$early/Contents/Info.plist" ]; then
    mkdir -p "$(dirname "$early")"
    ditto --norsrc "$root/WTLArrowEarlyClaim.kext" "$early"
fi
cmp "$root/WTLArrowEarlyClaim.kext/Contents/MacOS/WTLArrowEarlyClaim" "$early/Contents/MacOS/WTLArrowEarlyClaim"
cmp "$root/NativePowerTrial.1/AirportRTW.kext/Contents/MacOS/AirportRTW" "$esp_mp/EFI/OC/Kexts/WTLNativePowerTrial1/AirportRTW.kext/Contents/MacOS/AirportRTW"
cp "$trial/restore-efi-only.before.sh" "$root/restore-efi-only.sh"
chown root:wheel "$root/restore-efi-only.sh"
chmod 755 "$root/restore-efi-only.sh"
write_collection "$root/Arrow-Native-candidate.kc"
nvram -x -f "$root/gates.plist"
"$root/gate-audit"
write_config "$trial/config.d3.plist"
sysctl -n kern.bootsessionuuid > "$root/prepared-boot"
unmount_esp
trap - EXIT
echo D3_AND_4K_NEXT_BOOT_RESTORED_NO_RESTART
