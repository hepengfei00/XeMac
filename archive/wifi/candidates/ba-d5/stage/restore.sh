#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
[ "$(id -u)" = 0 ]
root='/Library/Application Support/WTLArrowTiming.EXAMPLE'
trial="$root/NativeBATrial.1"
. "$root/esp.sh"
mount_esp
trap unmount_esp EXIT
if cmp -s "$esp_config" "$trial/config.wifi.plist"; then
 write_config "$trial/config.before.plist"
else
 cmp -s "$esp_config" "$trial/config.before.plist"
fi
if cmp -s "$root/restore-efi-only.sh" "$trial/restore-efi-only.wifi.sh"; then
 cp "$trial/restore-efi-only.before.sh" "$root/restore-efi-only.sh.wifi-restore"
 chown root:wheel "$root/restore-efi-only.sh.wifi-restore"
 chmod 755 "$root/restore-efi-only.sh.wifi-restore"
 mv -f "$root/restore-efi-only.sh.wifi-restore" "$root/restore-efi-only.sh"
else
 cmp "$root/restore-efi-only.sh" "$trial/restore-efi-only.before.sh"
fi
job=/Library/LaunchDaemons/org.local.wtl-native-ba1-once.plist
[ ! -f "$job" ] || mv "$job" "$trial/collector-cancelled.plist"
unmount_esp
trap - EXIT
echo WIFI_TEST_CANCELLED_VERIFIED_4K_NEXT_BOOT_RETAINED
