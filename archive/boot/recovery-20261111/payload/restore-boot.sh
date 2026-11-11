#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
test "$(id -u)" = 0
root=$(cd "$(dirname "$0")" && pwd)
. "$root/esp.sh"
mount_esp
trap unmount_esp EXIT
if ! cmp -s "$esp_config" "$root/stable-config.plist"; then
    cmp -s "$esp_config" "$root/trial-config.plist"
    write_config "$root/stable-config.plist"
fi
unmount_esp
trap - EXIT
write_collection "$root/AuxiliaryKernelExtensions-before.kc"
echo NEXT_BOOT_ORIGINAL_EFI_AND_COLLECTION_RESTORED
