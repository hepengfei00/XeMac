#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
[ "$(id -u)" = 0 ]
[ "$(sysctl -n kern.bootsessionuuid)" = F3F3C04C-D34E-5D94-BD17-6ACF60B73910 ]
stage=$(cd "$(dirname "$0")" && pwd)
root='/Library/Application Support/WTLArrowTiming.EXAMPLE'
backup="$root/EarlyClaimRescueRepair.1"
sha() { shasum -a 256 "$1" | awk '{print $1}'; }
[ "$(sha "$root/trial-config.plist")" = 0918216c513d2d1431b11c9e46bd44cedb95a03bf6fc76ae5eb77c4108b14510 ]
[ "$(sha "$root/WTLArrowEarlyClaim.kext/Contents/MacOS/WTLArrowEarlyClaim")" = 20f252e9c5ae35b021d3735715ab501a974d16e7952edb081998528070390638 ]
[ "$(sha "$root/NativePowerTrial.1/AirportRTW.kext/Contents/MacOS/AirportRTW")" = 04ff7098bdb66e66d679e117b8ce86609d72d3a89322380614c777aa360a825b ]
[ "$(sha "$root/onboot-verified.sh")" = 73ceb7ebb2769a9d209b362a32c99b1ab8bd7de6f4892ba83946ba2d0946a718 ]
[ "$(sha /Library/KernelCollections/AuxiliaryKernelExtensions.kc)" = 910da863fd31d575d792fdd3f2bb2bf59c295010fe2cc2a584e6ceffaf52fa27 ]
[ "$(sha /Library/GPUBundles/XeMTLDriver.bundle/Contents/MacOS/XeMTLDriver)" = 6f2e9c5b6306ee132ccca967e911a42d904c85f5f94917f6b65066fd007de696 ]
for name in libLLVM.dylib libzstd.1.dylib; do
 [ -s "/Library/GPUBundles/XeMTLDriver.bundle/Contents/MacOS/air-runtime/$name" ]
done
codesign --verify --deep --strict "$root/WTLArrowEarlyClaim.kext"
codesign --verify --deep --strict "$root/NativePowerTrial.1/AirportRTW.kext"
codesign --verify --deep --strict /Library/GPUBundles/XeMTLDriver.bundle
"$root/ocvalidate" "$root/trial-config.plist"
[ ! -e "$backup" ]
mkdir -m 700 "$backup"
ditto --norsrc "$stage" "$backup/payload"
. "$root/esp.sh"
mount_esp
trap unmount_esp EXIT
[ "$(diskutil info -plist "$esp_part" | plutil -extract DiskUUID raw -o - -)" = 00000000-0000-0000-0000-000000000001 ]
cmp -s "$esp_config" "$root/trial-config.plist"
ditto --norsrc "$esp_mp/EFI" "$backup/EFI-before"
diff -qr -x '._*' "$esp_mp/EFI" "$backup/EFI-before" > "$backup/backup-verified.txt"
cp "$root/prepared-boot" "$backup/prepared-boot.before"
nvram -x -p > "$backup/nvram-before.plist"
old_missing=0
while IFS='|' read -r kind relative; do
 [ -s "$esp_mp/EFI/OC/$relative" ] || { echo "MISSING_BEFORE=$relative"; old_missing=$((old_missing+1)); }
done < "$stage/enabled-files.txt"
printf 'MISSING_BEFORE_COUNT=%s\n' "$old_missing"
parent="$esp_mp/EFI/OC/Kexts/WTLArrowTrial3"
dest="$parent/WTLArrowEarlyClaim.kext"
[ ! -e "$parent.rescue-new" ]
mkdir "$parent.rescue-new"
ditto --norsrc "$root/WTLArrowEarlyClaim.kext" "$parent.rescue-new/WTLArrowEarlyClaim.kext"
diff -qr -x '._*' "$root/WTLArrowEarlyClaim.kext" "$parent.rescue-new/WTLArrowEarlyClaim.kext"
if [ -e "$parent" ]; then mv "$parent" "$parent.rescue-before"; fi
mv "$parent.rescue-new" "$parent"
[ "$(sha "$dest/Contents/MacOS/WTLArrowEarlyClaim")" = 20f252e9c5ae35b021d3735715ab501a974d16e7952edb081998528070390638 ]
diff -qr -x '._*' "$root/WTLArrowEarlyClaim.kext" "$dest"
[ "$(sha "$esp_mp/EFI/OC/Kexts/WTLNativePowerTrial1/AirportRTW.kext/Contents/MacOS/AirportRTW")" = 04ff7098bdb66e66d679e117b8ce86609d72d3a89322380614c777aa360a825b ]
count=0
: > "$backup/enabled-files.sha256"
while IFS='|' read -r kind relative; do
 file="$esp_mp/EFI/OC/$relative"
 [ -s "$file" ] || { echo "MISSING_AFTER=$relative"; exit 20; }
 [ "$kind" != plist ] || plutil -lint "$file"
 shasum -a 256 "$file" >> "$backup/enabled-files.sha256"
 count=$((count+1))
done < "$stage/enabled-files.txt"
[ -s "$esp_mp/EFI/BOOT/BOOTx64.efi" ]
"$root/ocvalidate" "$esp_config"
cmp -s "$esp_config" "$root/trial-config.plist"
# Rescue boot did not initialize the GPU. Re-arm the original gates for the
# repaired internal profile; no driver load or graphical-session restart here.
nvram -x -f "$root/gates.plist"
"$root/gate-audit" > "$backup/rearmed-gates.txt"
sysctl -n kern.bootsessionuuid > "$root/prepared-boot"
shasum -a 256 "$esp_config" "$dest/Contents/Info.plist" "$dest/Contents/MacOS/WTLArrowEarlyClaim" /Library/KernelCollections/AuxiliaryKernelExtensions.kc > "$backup/ready.sha256"
cat "$backup/ready.sha256" "$backup/rearmed-gates.txt"
printf 'ENABLED_BOOT_FILES_CHECKED=%s\n' "$count"
sync
unmount_esp
trap - EXIT
echo EARLYCLAIM_REPAIRED_D3_PROFILE_CHECKED_NO_RESTART
