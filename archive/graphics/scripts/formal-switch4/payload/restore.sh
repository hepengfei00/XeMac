#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
root=$(cd "$(dirname "$0")" && pwd)
bash "$root/restore-boot.sh"
if [ -f "$root/keys-were-absent" ]; then
    while read -r key value; do nvram -d "$key" >/dev/null 2>&1 || true; done < "$root/gates.txt"
    nvram -d xegfx-disp-bootmode >/dev/null 2>&1 || true
fi
if [ -f "$root/userland-installed" ]; then
    for name in XeMTLDriver.bundle xemtl xemtl-allow.txt .xemtl-fbfetch-ws; do
        dest="/Library/GPUBundles/$name"
        if [ -e "$root/userland-before/$name" ]; then
            rm -rf "$dest.arrow-restore"
            ditto --norsrc "$root/userland-before/$name" "$dest.arrow-restore"
            rm -rf "$dest"
            mv "$dest.arrow-restore" "$dest"
        elif [ -f "$root/absent-$name" ]; then
            rm -rf "$dest"
        fi
    done
fi
. "$root/esp.sh"
mount_esp
trap unmount_esp EXIT
if [ -d "$esp_mp/EFI/OC/Kexts/WTLArrowTrial3" ]; then
    cmp -s "$root/WTLArrowEarlyClaim.kext/Contents/MacOS/WTLArrowEarlyClaim" "$esp_mp/EFI/OC/Kexts/WTLArrowTrial3/WTLArrowEarlyClaim.kext/Contents/MacOS/WTLArrowEarlyClaim"
    rm -rf "$esp_mp/EFI/OC/Kexts/WTLArrowTrial3"
fi
unmount_esp
trap - EXIT
echo ORIGINAL_NEXT_BOOT_AND_USERLAND_RESTORED_NO_REBOOT_REQUESTED
