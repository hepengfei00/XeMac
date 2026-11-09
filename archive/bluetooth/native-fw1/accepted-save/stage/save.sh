#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
trap 'rc=$?; echo "ACCEPTED_SAVE_ERROR_LINE=$LINENO EXIT=$rc"; exit "$rc"' ERR
[ "$(id -u)" = 0 ]
[ "$(sysctl -n kern.bootsessionuuid)" = E7B590BE-CE6B-5E92-B72C-BE812951B133 ]
[ "$(sw_vers -buildVersion)" = 25G332 ]
root='/Library/Application Support/WTLArrowTiming.EXAMPLE'
stage=$(cd "$(dirname "$0")" && pwd)
saved="$root/BluetoothAccepted.1"
sha(){ shasum -a 256 "$1" | awk '{print $1}'; }
cd "$stage"
shasum -a 256 --strict --quiet -c SHA256SUMS
bash "$root/health.sh"
kmutil showloaded --list-only > "$stage/loaded-private.txt" 2>/dev/null
for identity in EF620012-D012-378D-8FAB-F0DDE3FFD7F0 12F597FD-7523-3B61-B967-635DA9010B62 721AE88F-E9E1-3AEB-BE1E-0E23CB013985; do grep -q "$identity" "$stage/loaded-private.txt"; done
[ "$(sha "$root/trial-config.plist")" = 0918216c513d2d1431b11c9e46bd44cedb95a03bf6fc76ae5eb77c4108b14510 ]
[ "$(sha "$root/restore-efi-only.sh")" = 2fab6bafd51f2a87b1f30d95089691d2407a67c5c1d369ecf219ed67568d1db5 ]
[ "$(sha /Library/KernelCollections/AuxiliaryKernelExtensions.kc)" = 910da863fd31d575d792fdd3f2bb2bf59c295010fe2cc2a584e6ceffaf52fa27 ]
[ "$(sha "$stage/config.accepted.plist")" = c0cf8993dc4926da6031e03699529e02c00a1aa5ae2842aa413d6f5d3162bf68 ]
./ocvalidate config.accepted.plist
[ ! -e "$saved" ]
mkdir -m 700 "$saved"
. "$root/esp.sh"
mount_esp
changed=0
completed=0
finish(){
 rc=$?
 trap - EXIT
 set +e
 if [ "$changed" = 1 ] && [ "$completed" = 0 ]; then
  cp "$saved/trial-config.before.plist" "$root/trial-config.plist.accepted-rollback"
  chown root:wheel "$root/trial-config.plist.accepted-rollback"
  chmod 644 "$root/trial-config.plist.accepted-rollback"
  mv -f "$root/trial-config.plist.accepted-rollback" "$root/trial-config.plist"
  write_config "$saved/config.before.plist"
  cp "$saved/stable-enabled.before.sha256" "$root/stable-enabled.sha256"
  echo SAVE_FAILED_ORIGINAL_D3_BOOT_PROFILE_RESTORED_NO_RESTART
 fi
 unmount_esp
 exit "$rc"
}
trap finish EXIT
cmp "$esp_config" "$root/trial-config.plist"
[ "$(diskutil info -plist "$esp_part" | plutil -extract DiskUUID raw -o - -)" = 00000000-0000-0000-0000-000000000001 ]
need_kb=$(du -sk "$esp_mp/EFI" | awk '{print $1}')
free_kb=$(df -k "$root" | tail -1 | awk '{print $4}')
[ "$free_kb" -ge "$((need_kb*2+65536))" ]
ditto --norsrc "$esp_mp/EFI" "$saved/EFI-before"
diff -qr -x '._*' "$esp_mp/EFI" "$saved/EFI-before" > "$saved/efi-before-verified.txt"
cp "$esp_config" "$saved/config.before.plist"
cp "$root/trial-config.plist" "$saved/trial-config.before.plist"
cp "$root/stable-enabled.sha256" "$saved/stable-enabled.before.sha256"
cp "$root/restore-efi-only.sh" "$saved/restore-helper.before.sh"
count=0
while IFS='|' read -r kind relative; do
 file="$esp_mp/EFI/OC/$relative"
 [ -s "$file" ] || { echo "MISSING_ENABLED_FILE=$relative"; exit 20; }
 [ "$kind" != plist ] || plutil -lint "$file"
 count=$((count+1))
done < "$stage/enabled-files.txt"
[ "$count" = 29 ]
[ "$(sha "$esp_mp/EFI/OC/Kexts/WTLBluetoothTrial5/BlueToolFixup.kext/Contents/MacOS/BlueToolFixup")" = 9f5e2391f930a0d25a6ec946c42e3b73e7921fd6bce767b8f7c2676888851cb2 ]
[ "$(sha "$esp_mp/EFI/OC/Kexts/WTLBluetoothTrial2/RealtekBluetoothFirmware.kext/Contents/MacOS/RealtekBluetoothFirmware")" = 01c25091c752575a16667f1a4c12fc65ab92decabb71f1963baca2c4e9be1b16 ]
[ "$(sha "$esp_mp/EFI/OC/Kexts/WTLNativePowerTrial1/AirportRTW.kext/Contents/MacOS/AirportRTW")" = 04ff7098bdb66e66d679e117b8ce86609d72d3a89322380614c777aa360a825b ]
[ "$(sha "$esp_mp/EFI/OC/Kexts/WTLArrowTrial3/WTLArrowEarlyClaim.kext/Contents/MacOS/WTLArrowEarlyClaim")" = 20f252e9c5ae35b021d3735715ab501a974d16e7952edb081998528070390638 ]
codesign --verify --deep --strict "$esp_mp/EFI/OC/Kexts/WTLBluetoothTrial5/BlueToolFixup.kext"
codesign --verify --deep --strict "$esp_mp/EFI/OC/Kexts/WTLBluetoothTrial2/RealtekBluetoothFirmware.kext"
cp "$stage/config.accepted.plist" "$saved/config.accepted.plist"
cp "$stage/restore-d3.sh" "$saved/restore-d3.sh"
chmod 700 "$saved/restore-d3.sh"
changed=1
cp "$stage/config.accepted.plist" "$root/trial-config.plist.accepted-new"
chown root:wheel "$root/trial-config.plist.accepted-new"
chmod 644 "$root/trial-config.plist.accepted-new"
mv -f "$root/trial-config.plist.accepted-new" "$root/trial-config.plist"
write_config "$root/trial-config.plist"
./ocvalidate "$esp_config"
cmp "$esp_config" "$stage/config.accepted.plist"
cmp "$root/trial-config.plist" "$stage/config.accepted.plist"
[ "$(sha "$root/restore-efi-only.sh")" = 2fab6bafd51f2a87b1f30d95089691d2407a67c5c1d369ecf219ed67568d1db5 ]
[ "$(sha /Library/KernelCollections/AuxiliaryKernelExtensions.kc)" = 910da863fd31d575d792fdd3f2bb2bf59c295010fe2cc2a584e6ceffaf52fa27 ]
shasum -a 256 "$esp_config" /Library/KernelCollections/AuxiliaryKernelExtensions.kc > "$root/stable-enabled.sha256"
cp "$root/stable-enabled.sha256" "$saved/accepted-enabled.sha256"
ditto --norsrc "$esp_mp/EFI" "$saved/EFI-accepted"
diff -qr -x '._*' "$esp_mp/EFI" "$saved/EFI-accepted" > "$saved/efi-accepted-verified.txt"
sysctl -n kern.bootsessionuuid > "$saved/accepted-running-boot.txt"
printf 'BlueToolFixup=2.7.2d2\nRealtekFirmware=1.0.2d1\nUserAccepted=Yes\nNextBootProfile=accepted-BT-d3-4K\n' > "$saved/ACCEPTED.txt"
bash "$root/health.sh"
# End our one-time collector; it is not part of Bluetooth operation.
job=/Library/LaunchDaemons/org.local.wtl-bluetooth-trial5-once.plist
if [ -f "$job" ]; then
 cp "$job" "$saved/diagnostic-collector.before.plist"
 launchctl bootout system/org.local.wtl-bluetooth-trial5-once >/dev/null 2>&1 || true
 rm -f "$job"
fi
sync
cat "$saved/accepted-enabled.sha256"
echo ACCEPTED_ENABLED_FILES_CHECKED="$count"
completed=1
unmount_esp
trap - EXIT
echo BT_ACCEPTED_SAVED_FOR_FUTURE_BOOTS_NO_RESTART
