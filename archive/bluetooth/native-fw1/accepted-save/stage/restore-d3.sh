#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
[ "$(id -u)" = 0 ]
root='/Library/Application Support/WTLArrowTiming.EXAMPLE'
saved="$root/BluetoothAccepted.1"
. "$root/esp.sh"
mount_esp
trap unmount_esp EXIT
if ! cmp -s "$esp_config" "$saved/config.accepted.plist"; then cmp "$esp_config" "$saved/config.before.plist"; fi
cp "$saved/trial-config.before.plist" "$root/trial-config.plist.restore-new"
chown root:wheel "$root/trial-config.plist.restore-new"
chmod 644 "$root/trial-config.plist.restore-new"
mv -f "$root/trial-config.plist.restore-new" "$root/trial-config.plist"
write_config "$saved/config.before.plist"
for key in bluetoothInternalControllerInfo bluetoothExternalDongleFailed bluetoothHostControllerSwitchBehavior; do
 if [ -f "$root/BluetoothTrial.4/$key.before.plist" ]; then nvram -x -f "$root/BluetoothTrial.4/$key.before.plist"; else nvram -d "$key" >/dev/null 2>&1 || true; fi
done
nvram -x -f "$root/gates.plist"
"$root/gate-audit"
shasum -a 256 "$esp_config" /Library/KernelCollections/AuxiliaryKernelExtensions.kc > "$root/stable-enabled.sha256"
unmount_esp
trap - EXIT
echo D3_4K_NEXT_BOOT_RESTORED_WITH_ORIGINAL_BT_FLAGS_NO_RESTART
