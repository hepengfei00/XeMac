#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
[ "$(id -u)" = 0 ]
[ "$(sysctl -n kern.bootsessionuuid)" = E855C422-C1CF-54EA-8518-F8B2D44A4D7B ]
stage=$(cd "$(dirname "$0")" && pwd)
root='/Library/Application Support/WTLArrowTiming.EXAMPLE'
cd "$stage"
shasum -a 256 --strict --quiet -c SHA256SUMS
[ -z "$(kmutil showloaded --list-only 2>/dev/null | grep -E 'com.senmiko.AirportRTW|org.local.WTLRTW89BackendDiag' || true)" ]
bash "$root/health.sh"
[ "$(shasum -a 256 "$root/restore-efi-only.sh" | awk '{print $1}')" = 2fab6bafd51f2a87b1f30d95089691d2407a67c5c1d369ecf219ed67568d1db5 ]
cmp "$root/Arrow-Native-candidate.kc" /Library/KernelCollections/AuxiliaryKernelExtensions.kc
[ "$(df -k "$root" | tail -1 | awk '{print $4}')" -gt 307200 ]
backup=$(mktemp -d "$root/ArchivedInactiveWiFiEFI.XXXXXX")
chmod 700 "$backup"
printf '%s\n' "$backup" > "$stage/efi-backup-path.txt"
. "$root/esp.sh"
mount_esp
trap unmount_esp EXIT
cmp config.before.plist "$esp_config"
df -k "$esp_mp" > "$backup/space-before.txt"
for config in "$esp_config" "$root/stable-config.plist" "$root/trial-config.plist"; do
    plutil -extract Kernel.Add xml1 -o - "$config" >> "$backup/kernel-add-profiles.xml"
done
while IFS=$'\t' read -r name binary expected; do
    case "$name" in WTLWiFi231Trial1|WTLWiFi231Trial2|WTLBackendTrial1|WTLNativePowerTrial1|WTLNativeRxTrial1) ;; *) exit 1;; esac
    from="$esp_mp/EFI/OC/Kexts/$name"
    [ -d "$from" ] || continue
    if grep -Fq "$name/" "$backup/kernel-add-profiles.xml"; then echo "SKIP_REFERENCED=$name"; continue; fi
    got=$(shasum -a 256 "$from/$binary" | awk '{print $1}')
    if [ "$got" != "$expected" ]; then echo "SKIP_UNKNOWN_BINARY=$name"; continue; fi
    ditto --norsrc "$from" "$backup/$name"
    diff -qr -x '._*' "$from" "$backup/$name"
    printf '%s  %s\n' "$got" "$name/$binary" >> "$backup/verified-binaries.sha256"
    rm -rf -- "$from"
    echo "ARCHIVED_AND_FREED=$name"
done < "$stage/retire-allowlist.tsv"
name=WTLNativeBATrial1
from="$esp_mp/EFI/OC/Kexts/$name"
if [ -d "$from" ]; then
    if grep -Fq "$name/" "$backup/kernel-add-profiles.xml"; then
        echo REFUSE_REFERENCED_PARTIAL_DIRECTORY
        exit 1
    fi
    [ "$(cat "$root/NativeBATrial.1/prepared-boot")" = E855C422-C1CF-54EA-8518-F8B2D44A4D7B ]
    [ "$(shasum -a 256 "$root/NativeBATrial.1/AirportRTW.kext/Contents/MacOS/AirportRTW" | awk '{print $1}')" = acdd870da84b7dd4892b101d7f1c67fdc730e68ca0ca6ebdef86c1e413d8df27 ]
    ditto --norsrc "$from" "$backup/$name.partial"
    diff -qr -x '._*' "$from" "$backup/$name.partial"
    rm -rf -- "$from"
    echo PARTIAL_D5_COPY_BACKED_UP_AND_FREED
fi
df -k "$esp_mp" > "$backup/space-after.txt"
cat "$backup/space-before.txt" "$backup/space-after.txt"
unmount_esp
trap - EXIT
if [ -d "$root/NativeBATrial.1" ]; then
    [ "$(cat "$root/NativeBATrial.1/prepared-boot")" = E855C422-C1CF-54EA-8518-F8B2D44A4D7B ]
    mv "$root/NativeBATrial.1" "$backup/failed-root-trial"
fi
bash "$stage/install.sh"
bash "$root/health.sh"
[ "$(sysctl -n kern.bootsessionuuid)" = E855C422-C1CF-54EA-8518-F8B2D44A4D7B ]
echo D5_SPEED_DIAG_READY_AFTER_VERIFIED_EFI_BACKUP_NO_RESTART
