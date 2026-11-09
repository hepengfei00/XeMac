#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
test "$(id -u)" = 0
root=$(cd "$(dirname "$0")" && pwd)
. "$root/esp.sh"
mount_esp
trap unmount_esp EXIT
cp "$esp_config" "$root/config-before-boot-restore.plist"
shopt -s nullglob
logs=("$esp_mp"/opencore-*.txt)
if [ "${#logs[@]}" -gt 0 ]; then latest=$(ls -t "${logs[@]}" | head -1); cp "$latest" "$root/actual-opencore-boot.log"; fi
if ! cmp -s "$esp_config" "$root/stable-config.plist"; then
    if cmp -s "$esp_config" "$root/NativeBATrial.1/config.wifi.plist"; then
        write_config "$root/stable-config.plist"
        cp "$root/NativeBATrial.1/restore-efi-only.before.sh" "$root/restore-efi-only.sh.wifi-complete"
        chown root:wheel "$root/restore-efi-only.sh.wifi-complete"
        chmod 755 "$root/restore-efi-only.sh.wifi-complete"
        mv -f "$root/restore-efi-only.sh.wifi-complete" "$root/restore-efi-only.sh"
        echo WIFI_ONE_BOOT_ACCEPTED_GRAPHICS_BASELINE_RESTORED
    else
        cmp -s "$esp_config" "$root/trial-config.plist"
        write_config "$root/stable-config.plist"
    fi
fi
unmount_esp
trap - EXIT
echo NEXT_BOOT_ORIGINAL_EFI_RESTORED_CANDIDATE_CACHE_RETAINED_FOR_EXPLICIT_LOAD
