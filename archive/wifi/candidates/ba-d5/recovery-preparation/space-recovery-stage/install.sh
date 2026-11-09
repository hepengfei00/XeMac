#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
stage=$(cd "$(dirname "$0")" && pwd)
root='/Library/Application Support/WTLArrowTiming.EXAMPLE'
trial="$root/NativeBATrial.1"
[ "$(id -u)" = 0 ]
[ "$(sw_vers -buildVersion)" = 25G332 ]
[ "$(sysctl -n kern.bootsessionuuid)" = E855C422-C1CF-54EA-8518-F8B2D44A4D7B ]
cd "$stage"
shasum -a 256 --strict --quiet -c SHA256SUMS
shasum -a 256 --strict --quiet -c graphics-baseline.sha256
cmp config.before.plist "$root/trial-config.plist"
cmp "$root/Arrow-Native-candidate.kc" /Library/KernelCollections/AuxiliaryKernelExtensions.kc
[ "$(shasum -a 256 "$root/Arrow-Native-candidate.kc" | awk '{print $1}')" = 910da863fd31d575d792fdd3f2bb2bf59c295010fe2cc2a584e6ceffaf52fa27 ]
[ "$(shasum -a 256 "$root/restore-efi-only.sh" | awk '{print $1}')" = 2fab6bafd51f2a87b1f30d95089691d2407a67c5c1d369ecf219ed67568d1db5 ]
sysctl -n kern.bootargs | grep -q 'amfi=0x80'
bash "$root/health.sh"
# Recovery boot has no native/isolated wireless module. Do not mix classes.
loaded_wireless=$(kmutil showloaded --list-only 2>/dev/null | grep -E 'com.senmiko.AirportRTW|org.local.WTLRTW89BackendDiag' || true)
[ -z "$loaded_wireless" ]
codesign --verify AirportRTW.kext
[ "$(/usr/libexec/PlistBuddy -c Print:CFBundleVersion AirportRTW.kext/Contents/Info.plist)" = 2.3.1d5 ]
./ocvalidate config.wifi.plist
[ ! -e "$trial" ]
[ ! -e /Library/LaunchDaemons/org.local.wtl-native-ba1-once.plist ]
. "$root/esp.sh"
mount_esp
changed=0
completed=0
cleanup() {
 rc=$?
 trap - EXIT
 set +e
 if [ "$changed" = 1 ] && [ "$completed" = 0 ]; then
  write_config "$trial/config.before.plist"
  cp "$trial/restore-efi-only.before.sh" "$root/restore-efi-only.sh.rollback"
  chown root:wheel "$root/restore-efi-only.sh.rollback"
  chmod 755 "$root/restore-efi-only.sh.rollback"
  mv -f "$root/restore-efi-only.sh.rollback" "$root/restore-efi-only.sh"
  [ ! -f /Library/LaunchDaemons/org.local.wtl-native-ba1-once.plist ] || mv /Library/LaunchDaemons/org.local.wtl-native-ba1-once.plist "$trial/collector-not-armed.plist"
  echo WIFI_PREPARATION_FAILED_ORIGINAL_CONFIGURATION_RESTORED
 fi
 unmount_esp
 exit "$rc"
}
trap cleanup EXIT
cmp config.before.plist "$esp_config"
need_kb=$(du -sk "$stage/AirportRTW.kext" | awk '{print $1}')
free_kb=$(df -k "$esp_mp" | tail -1 | awk '{print $4}')
[ "$free_kb" -ge "$((need_kb + 8192))" ] || { echo EFI_SPACE_PREFLIGHT_FAILED_BEFORE_ANY_WRITE; exit 1; }
[ ! -e "$esp_mp/EFI/OC/Kexts/WTLNativeBATrial1" ]
mkdir -m 755 "$trial"
ditto --norsrc "$stage" "$trial"
chown -R root:wheel "$trial"
cp "$root/restore-efi-only.sh" "$trial/restore-efi-only.before.sh"
cp "$esp_config" "$trial/config.before.plist"
cp "$root/stable-config.plist" "$trial/graphics-fallback-config.plist"
sysctl -n kern.bootsessionuuid > "$trial/prepared-boot"
changed=1
mkdir "$esp_mp/EFI/OC/Kexts/WTLNativeBATrial1"
ditto --norsrc "$trial/AirportRTW.kext" "$esp_mp/EFI/OC/Kexts/WTLNativeBATrial1/AirportRTW.kext"
diff -qr -x '._*' "$trial/AirportRTW.kext" "$esp_mp/EFI/OC/Kexts/WTLNativeBATrial1/AirportRTW.kext"
cp "$trial/restore-efi-only.wifi.sh" "$root/restore-efi-only.sh.wifi-new"
chown root:wheel "$root/restore-efi-only.sh.wifi-new"
chmod 755 "$root/restore-efi-only.sh.wifi-new"
mv -f "$root/restore-efi-only.sh.wifi-new" "$root/restore-efi-only.sh"
cp "$trial/collector.plist" /Library/LaunchDaemons/org.local.wtl-native-ba1-once.plist
chown root:wheel /Library/LaunchDaemons/org.local.wtl-native-ba1-once.plist
chmod 644 /Library/LaunchDaemons/org.local.wtl-native-ba1-once.plist
write_config "$trial/config.wifi.plist"
cmp "$trial/config.wifi.plist" "$esp_config"
cmp "$root/Arrow-Native-candidate.kc" /Library/KernelCollections/AuxiliaryKernelExtensions.kc
shasum -a 256 "$esp_config" "$root/trial-config.plist" "$root/restore-efi-only.sh" /Library/KernelCollections/AuxiliaryKernelExtensions.kc > "$trial/enabled.sha256"
unmount_esp
completed=1
trap - EXIT
echo WIFI_DIAGNOSTIC_ONE_BOOT_READY_NO_RESTART_PERFORMED
