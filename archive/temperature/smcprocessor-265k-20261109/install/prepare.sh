#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
umask 077
[ "$(id -u)" = 0 ]
stage=$(cd "$(dirname "$0")" && pwd)
root='/Library/Application Support/WTLArrowTiming.EXAMPLE'
pkg="$root/CPUTemperature.1"
old=c0cf8993dc4926da6031e03699529e02c00a1aa5ae2842aa413d6f5d3162bf68
new=04eece81cf92b2c23e38e4e66cd2b44486df25ee18b6d02429fc408bfdc687b4
sensor_sha=58ea09935d5598e25ad00f965b87c470a47c7eca641c22470c537df6712dda31
hash() { shasum -a 256 "$1" | awk '{print $1}'; }
[ "$(sysctl -n kern.bootsessionuuid)" = 'E7B590BE-CE6B-5E92-B72C-BE812951B133' ]
[ "$(hash "$root/trial-config.plist")" = "$old" ]
[ ! -e "$pkg" ]
[ ! -e /Library/LaunchDaemons/org.local.wtl-cpu-temperature-check.plist ]
if ps -axo comm,args | grep -E '/(onboot-stable|onboot-verified|commit-next-boot)\.sh' | grep -v grep; then
    echo STARTUP_CONFIG_WRITER_ACTIVE_STOP_WITHOUT_INSTALLING
    exit 1
fi
bash "$root/health.sh"
mkdir "$pkg"
chmod 755 "$pkg"
completed=0
backup_ready=0
changed=0
. "$root/esp.sh"
cleanup() {
    rc=$?
    trap - EXIT
    set +e
    if [ "$completed" = 0 ]; then
        echo "INSTALL_STOPPED_EXIT=$rc"
        if [ "$changed" = 1 ] && [ "$backup_ready" = 1 ]; then
            bash "$pkg/rollback.sh"
            rollback_rc=$?
            echo "INSTALL_ROLLBACK_EXIT=$rollback_rc"
            [ "$rollback_rc" = 0 ] || echo RESTORE_REQUIRES_REVIEW_DO_NOT_RESTART
        fi
        unmount_esp
        touch "$pkg/INSTALL_FAILED"
    fi
    exit "$rc"
}
trap cleanup EXIT
while IFS=$'\t' read -r sha rel; do
    [ -n "$sha" ] && [ -n "$rel" ]
    case "$rel" in /*|*..*) echo INVALID_MANIFEST_PATH; exit 1;; esac
    [ "$(hash "$stage/$rel")" = "$sha" ]
    mkdir -p "$pkg/$(dirname "$rel")"
    cp "$stage/$rel" "$pkg/$rel"
    [ "$(hash "$pkg/$rel")" = "$sha" ]
done < "$stage/payload-files.tsv"
chown -R root:wheel "$pkg"
chmod 755 "$pkg/collect-nextboot.sh" "$pkg/rollback.sh" "$pkg/smcread" "$pkg/ocvalidate"
chmod 600 "$pkg/config.temperature.private.plist"
[ "$(hash "$pkg/config.temperature.private.plist")" = "$new" ]
[ "$(hash "$pkg/SMCProcessor.kext/Contents/MacOS/SMCProcessor")" = "$sensor_sha" ]
plutil -lint "$pkg/config.temperature.private.plist" "$pkg/SMCProcessor.kext/Contents/Info.plist" "$pkg/collector-job.plist"
"$pkg/ocvalidate" "$pkg/config.temperature.private.plist"
bash -n "$pkg/collect-nextboot.sh" "$pkg/rollback.sh"
mount_esp
[ "$(hash "$esp_config")" = "$old" ]
cmp -s "$root/trial-config.plist" "$esp_config"
[ ! -e "$esp_mp/EFI/OC/Kexts/SMCProcessor.kext" ]
[ ! -e "$esp_mp/EFI/OC/Kexts/SMCProcessor.kext.cpu-temperature-new" ]
[ ! -e "$esp_config.arrow-new" ]
[ ! -e "$root/trial-config.plist.cpu-temperature-new" ]
while IFS= read -r rel; do
    [ -f "$esp_mp/EFI/$rel" ]
done < "$pkg/baseline-enabled-files.txt"
needed=$(du -sk "$esp_mp/EFI" | awk '{print $1}')
available=$(df -Pk "$root" | awk 'END {print $4}')
[ "$available" -gt "$((needed + 32768))" ]
available=$(df -Pk "$esp_mp" | awk 'END {print $4}')
[ "$available" -gt 4096 ]
mkdir "$pkg/backup"
chmod 700 "$pkg/backup"
ditto --norsrc "$esp_mp/EFI" "$pkg/backup/EFI-before"
diff -qr "$esp_mp/EFI" "$pkg/backup/EFI-before"
cp -p "$root/trial-config.plist" "$pkg/backup/trial-config.plist"
[ "$(hash "$pkg/backup/trial-config.plist")" = "$old" ]
shasum -a 256 "$root/esp.sh" "$root/restore-efi-only.sh" "$root/commit-next-boot.sh" "$root/onboot-stable.sh" "$root/onboot-verified.sh" /Library/KernelCollections/AuxiliaryKernelExtensions.kc /Library/GPUBundles/XeMTLDriver.bundle/Contents/MacOS/XeMTLDriver > "$pkg/protected-before.sha256"
sysctl -n kern.bootsessionuuid > "$pkg/prepared-boot"
chmod 644 "$pkg/prepared-boot"
backup_ready=1
changed=1
ditto --norsrc "$pkg/SMCProcessor.kext" "$esp_mp/EFI/OC/Kexts/SMCProcessor.kext.cpu-temperature-new"
diff -qr "$pkg/SMCProcessor.kext" "$esp_mp/EFI/OC/Kexts/SMCProcessor.kext.cpu-temperature-new"
mv "$esp_mp/EFI/OC/Kexts/SMCProcessor.kext.cpu-temperature-new" "$esp_mp/EFI/OC/Kexts/SMCProcessor.kext"
[ "$(hash "$root/trial-config.plist")" = "$old" ]
cp -p "$root/trial-config.plist" "$root/trial-config.plist.cpu-temperature-new"
cat "$pkg/config.temperature.private.plist" > "$root/trial-config.plist.cpu-temperature-new"
cmp -s "$root/trial-config.plist.cpu-temperature-new" "$pkg/config.temperature.private.plist"
mv -f "$root/trial-config.plist.cpu-temperature-new" "$root/trial-config.plist"
write_config "$pkg/config.temperature.private.plist"
[ "$(hash "$root/trial-config.plist")" = "$new" ]
[ "$(hash "$esp_config")" = "$new" ]
while IFS= read -r rel; do
    [ -f "$esp_mp/EFI/$rel" ]
done < "$pkg/candidate-enabled-files.txt"
while IFS= read -r -d '' file; do
    rel=${file#"$pkg/backup/EFI-before/"}
    [ "$rel" = OC/config.plist ] || cmp -s "$file" "$esp_mp/EFI/$rel"
done < <(find "$pkg/backup/EFI-before" -type f -print0)
diff -qr "$pkg/SMCProcessor.kext" "$esp_mp/EFI/OC/Kexts/SMCProcessor.kext"
shasum -a 256 -c "$pkg/protected-before.sha256"
cp "$pkg/collector-job.plist" /Library/LaunchDaemons/org.local.wtl-cpu-temperature-check.plist
chown root:wheel /Library/LaunchDaemons/org.local.wtl-cpu-temperature-check.plist
chmod 644 /Library/LaunchDaemons/org.local.wtl-cpu-temperature-check.plist
plutil -lint /Library/LaunchDaemons/org.local.wtl-cpu-temperature-check.plist
mkdir -p "$pkg/results" "$pkg/collector-locks"
chmod 755 "$pkg/results" "$pkg/collector-locks"
# Install only. launchd will start this read-only collector at the next boot.
# Do not bootstrap, load the sensor, or restart anything in this session.
bash "$root/health.sh"
shasum -a 256 "$esp_config" "$root/trial-config.plist" "$esp_mp/EFI/OC/Kexts/SMCProcessor.kext/Contents/MacOS/SMCProcessor" > "$pkg/installed.sha256"
unmount_esp
sync
touch "$pkg/INSTALLED_FOR_NEXT_BOOT_NO_RESTART"
chmod 644 "$pkg/INSTALLED_FOR_NEXT_BOOT_NO_RESTART" "$pkg/installed.sha256" "$pkg/protected-before.sha256"
completed=1
trap - EXIT
echo CPU_TEMPERATURE_INSTALLED_NEXT_BOOT_COLLECTOR_READY_NO_RESTART
