#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
trap 'rc=$?; echo "PREPARATION_FAILED_LINE=$LINENO EXIT=$rc"; exit "$rc"' ERR
[ "$(id -u)" = 0 ]
[ "$(sysctl -n kern.bootsessionuuid)" = C0357155-D603-5C24-9A70-9264894902CB ]
stage=$(cd "$(dirname "$0")" && pwd)
root='/Library/Application Support/WTLArrowTiming.EXAMPLE'
trial="$root/BluetoothTrial.2"
sha() { shasum -a 256 "$1" | awk '{print $1}'; }
cd "$stage"
shasum -a 256 --strict --quiet -c SHA256SUMS
[ "$(sha "$root/trial-config.plist")" = 0918216c513d2d1431b11c9e46bd44cedb95a03bf6fc76ae5eb77c4108b14510 ]
[ "$(sha "$root/restore-efi-only.sh")" = 2fab6bafd51f2a87b1f30d95089691d2407a67c5c1d369ecf219ed67568d1db5 ]
[ "$(sha /Library/KernelCollections/AuxiliaryKernelExtensions.kc)" = 910da863fd31d575d792fdd3f2bb2bf59c295010fe2cc2a584e6ceffaf52fa27 ]
cmp "$root/trial-config.plist" "$stage/config.d3.plist"
if ! bash "$root/health.sh"; then
    bash -x "$root/health.sh"
fi
for name in BlueToolFixup RealtekBluetoothFirmware; do codesign --verify --deep --strict "$stage/$name.kext"; done
./ocvalidate config.bluetooth.plist
[ ! -e "$trial" ]
mkdir -m 755 "$trial"
ditto --norsrc "$stage" "$trial"
chown -R root:wheel "$trial"
chmod -R go-w "$trial"
cp "$root/restore-efi-only.sh" "$trial/restore-efi-only.before.sh"
cp "$root/prepared-boot" "$trial/prepared-boot.before"
. "$root/esp.sh"
mount_esp
completed=0
changed=0
finish() {
    rc=$?
    trap - EXIT
    set +e
    if [ "$changed" = 1 ] && [ "$completed" = 0 ]; then
        write_config "$trial/config.d3.plist"
        cp "$trial/restore-efi-only.before.sh" "$root/restore-efi-only.sh"
        chmod 755 "$root/restore-efi-only.sh"
        cp "$trial/prepared-boot.before" "$root/prepared-boot"
        while read -r key value; do
            if [ -f "$trial/$key.before.plist" ]; then nvram -x -f "$trial/$key.before.plist"; else nvram -d "$key" >/dev/null 2>&1; fi
        done < "$root/gates.txt"
        rm -f /Library/LaunchDaemons/org.local.wtl-bluetooth-trial2-once.plist
        echo BLUETOOTH_PREPARATION_FAILED_D3_PROFILE_RESTORED_NO_RESTART
    fi
    unmount_esp
    exit "$rc"
}
trap finish EXIT
[ "$(diskutil info -plist "$esp_part" | plutil -extract DiskUUID raw -o - -)" = 00000000-0000-0000-0000-000000000001 ]
cmp "$esp_config" "$trial/config.d3.plist"
dest="$esp_mp/EFI/OC/Kexts/WTLBluetoothTrial2"
[ ! -e "$dest" ]
need_kb=$(du -sk "$stage/BlueToolFixup.kext" "$stage/RealtekBluetoothFirmware.kext" | awk '{n+=$1} END {print n}')
free_kb=$(df -k "$esp_mp" | tail -1 | awk '{print $4}')
[ "$free_kb" -ge "$((need_kb+8192))" ]
ditto --norsrc "$esp_mp/EFI" "$trial/EFI-before"
diff -qr -x '._*' "$esp_mp/EFI" "$trial/EFI-before" > "$trial/efi-backup-verified.txt"
while read -r key value; do
    if ! nvram -x "$key" > "$trial/$key.before.plist" 2>/dev/null; then rm -f "$trial/$key.before.plist"; fi
done < "$root/gates.txt"
mkdir "$dest"
for name in BlueToolFixup RealtekBluetoothFirmware; do
    ditto --norsrc "$stage/$name.kext" "$dest/$name.kext"
    diff -qr -x '._*' "$stage/$name.kext" "$dest/$name.kext"
    codesign --verify --deep --strict "$dest/$name.kext"
done
count=0
: > "$trial/enabled-files.sha256"
while IFS='|' read -r kind relative; do
    file="$esp_mp/EFI/OC/$relative"
    [ -s "$file" ] || { echo "MISSING_ENABLED_FILE=$relative"; exit 20; }
    [ "$kind" != plist ] || plutil -lint "$file"
    shasum -a 256 "$file" >> "$trial/enabled-files.sha256"
    count=$((count+1))
done < "$trial/enabled-files.txt"
[ -s "$esp_mp/EFI/BOOT/BOOTx64.efi" ]
[ "$(sha "$esp_mp/EFI/OC/Kexts/WTLArrowTrial3/WTLArrowEarlyClaim.kext/Contents/MacOS/WTLArrowEarlyClaim")" = 20f252e9c5ae35b021d3735715ab501a974d16e7952edb081998528070390638 ]
[ "$(sha "$esp_mp/EFI/OC/Kexts/WTLNativePowerTrial1/AirportRTW.kext/Contents/MacOS/AirportRTW")" = 04ff7098bdb66e66d679e117b8ce86609d72d3a89322380614c777aa360a825b ]
changed=1
cp "$trial/restore-efi-only.bluetooth.sh" "$root/restore-efi-only.sh.bt-new"
chmod 755 "$root/restore-efi-only.sh.bt-new"
chown root:wheel "$root/restore-efi-only.sh.bt-new"
mv -f "$root/restore-efi-only.sh.bt-new" "$root/restore-efi-only.sh"
cp "$trial/collector.plist" /Library/LaunchDaemons/org.local.wtl-bluetooth-trial2-once.plist
chmod 644 /Library/LaunchDaemons/org.local.wtl-bluetooth-trial2-once.plist
chown root:wheel /Library/LaunchDaemons/org.local.wtl-bluetooth-trial2-once.plist
nvram -x -f "$root/gates.plist"
"$root/gate-audit" > "$trial/rearmed-gates.txt"
write_config "$trial/config.bluetooth.plist"
"$trial/ocvalidate" "$esp_config"
cmp "$esp_config" "$trial/config.bluetooth.plist"
cmp "$root/trial-config.plist" "$trial/config.d3.plist"
[ "$(sha /Library/KernelCollections/AuxiliaryKernelExtensions.kc)" = 910da863fd31d575d792fdd3f2bb2bf59c295010fe2cc2a584e6ceffaf52fa27 ]
sysctl -n kern.bootsessionuuid > "$root/prepared-boot"
cp "$root/prepared-boot" "$trial/prepared-boot"
shasum -a 256 "$esp_config" "$root/trial-config.plist" "$root/restore-efi-only.sh" "$dest/RealtekBluetoothFirmware.kext/Contents/MacOS/RealtekBluetoothFirmware" "$dest/BlueToolFixup.kext/Contents/MacOS/BlueToolFixup" > "$trial/enabled.sha256"
cat "$trial/enabled.sha256" "$trial/rearmed-gates.txt"
printf 'ENABLED_BOOT_FILES_CHECKED=%s\n' "$count"
if ! bash "$root/health.sh"; then
    bash -x "$root/health.sh"
fi
sync
completed=1
unmount_esp
trap - EXIT
echo BLUETOOTH_ONE_BOOT_PREPARED_D3_AND_4K_PRESERVED_NO_RESTART
