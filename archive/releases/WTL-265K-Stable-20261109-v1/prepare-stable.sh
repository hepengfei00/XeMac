#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
umask 077
[ "$(id -u)" = 0 ]
stage=$(cd "$(dirname "$0")" && pwd)
root='/Library/Application Support/WTLArrowTiming.EXAMPLE'
store='/Library/Application Support/WTLStable'
id='WTL-265K-Stable-20261109-v1'
pkg="$store/$id"
old=e1c8fc66fd293746521941dab468b0a68a07dacd13b15389bbd2d69e6ff1634e
new=ead26ddfb4e249dab2d85fb53a7918d73da58977d0b33cb2e719fcfc45a51dac
hash() { shasum -a 256 "$1" | awk '{print $1}'; }
[ "$(sysctl -n kern.bootsessionuuid)" = '0B5E5308-7587-5322-8D93-1731920FD775' ]
[ "$(hash "$root/trial-config.plist")" = "$old" ]
[ ! -e "$pkg" ]
if ps -axo comm,args | grep -E '/(onboot-stable|onboot-verified|commit-next-boot)\.sh' | grep -v grep; then
    echo STARTUP_CONFIG_WRITER_ACTIVE_STOP
    exit 1
fi
bash "$root/health.sh"
mkdir -p "$store"
chmod 755 "$store"
mkdir "$pkg"
chmod 755 "$pkg"
for file in config.debug.private.plist config.quiet.private.plist toggle-debug.sh ocvalidate README.md manifest.json; do
    cp "$stage/$file" "$pkg/$file"
    cmp -s "$stage/$file" "$pkg/$file"
done
chown -R root:wheel "$pkg"
chmod 755 "$pkg/toggle-debug.sh" "$pkg/ocvalidate"
chmod 644 "$pkg/README.md" "$pkg/manifest.json"
[ "$(hash "$pkg/config.debug.private.plist")" = "$old" ]
[ "$(hash "$pkg/config.quiet.private.plist")" = "$new" ]
"$pkg/ocvalidate" "$pkg/config.quiet.private.plist"
bash -n "$pkg/toggle-debug.sh"
. "$root/esp.sh"
completed=0
changed=0
cleanup() {
    rc=$?
    trap - EXIT
    set +e
    if [ "$completed" = 0 ]; then
        echo "STABLE_PREPARATION_STOPPED_EXIT=$rc"
        if [ "$changed" = 1 ]; then
            rm -f "$root/trial-config.plist.quiet-new" "${esp_config:-/nonexistent}.arrow-new"
            bash "$pkg/toggle-debug.sh" on
            echo "RESTORE_PREVIOUS_DEBUG_CONFIGURATION_EXIT=$?"
        fi
        unmount_esp
        touch "$pkg/PREPARATION_FAILED"
    fi
    exit "$rc"
}
trap cleanup EXIT
mount_esp
[ "$(hash "$esp_config")" = "$old" ]
cmp -s "$root/trial-config.plist" "$esp_config"
[ ! -e "$root/trial-config.plist.quiet-new" ]
[ ! -e "$root/trial-config.plist.debug-switch" ]
[ ! -e "$esp_config.arrow-new" ]
needed=$(du -sk "$root" /Library/GPUBundles /Library/KernelCollections/AuxiliaryKernelExtensions.kc "$esp_mp/EFI" | awk '{sum+=$1} END {print sum}')
available=$(df -Pk "$root" | awk 'END {print $4}')
echo "SNAPSHOT_SOURCE_KIB=$needed AVAILABLE_KIB=$available"
[ "$available" -gt "$((needed * 4 + 262144))" ]
mkdir "$pkg/before"
ditto --norsrc "$esp_mp/EFI" "$pkg/before/EFI"
diff -qr "$esp_mp/EFI" "$pkg/before/EFI"
cp -p "$root/trial-config.plist" "$pkg/before/trial-config.plist"
shasum -a 256 "$root/esp.sh" "$root/onboot-stable.sh" "$root/onboot-verified.sh" "$root/commit-next-boot.sh" "$root/restore-efi-only.sh" /Library/KernelCollections/AuxiliaryKernelExtensions.kc /Library/GPUBundles/XeMTLDriver.bundle/Contents/MacOS/XeMTLDriver /Library/LaunchDaemons/org.local.wtl-arrow-native-stable.plist /Library/LaunchDaemons/org.local.wtl-cpu-temperature-check.plist > "$pkg/protected-before.sha256"
echo PHASE_INSTALL_QUIET_CONFIGURATION
changed=1
cp -p "$root/trial-config.plist" "$root/trial-config.plist.quiet-new"
cat "$pkg/config.quiet.private.plist" > "$root/trial-config.plist.quiet-new"
cmp -s "$root/trial-config.plist.quiet-new" "$pkg/config.quiet.private.plist"
mv -f "$root/trial-config.plist.quiet-new" "$root/trial-config.plist"
write_config "$pkg/config.quiet.private.plist"
[ "$(hash "$esp_config")" = "$new" ]
[ "$(hash "$root/trial-config.plist")" = "$new" ]
[ "$(plutil -extract Misc.Boot.Timeout raw -o - "$esp_config")" = 3 ]
while IFS= read -r -d '' file; do
    rel=${file#"$pkg/before/EFI/"}
    [ "$rel" = OC/config.plist ] || cmp -s "$file" "$esp_mp/EFI/$rel"
done < <(find "$pkg/before/EFI" -type f -print0)
shasum -a 256 -c "$pkg/protected-before.sha256"
bash "$root/health.sh"
echo PHASE_SNAPSHOT_CURRENT_EFI_AND_DRIVERS
snapshot="$pkg/snapshot"
mkdir "$snapshot"
ditto --norsrc "$esp_mp/EFI" "$snapshot/EFI"
diff -qr "$esp_mp/EFI" "$snapshot/EFI"
shasum -a 256 "$esp_config" "$root/trial-config.plist" > "$pkg/installed.sha256"
unmount_esp
ditto --norsrc "$root" "$snapshot/DriverPackage"
ditto --norsrc /Library/GPUBundles "$snapshot/GPUBundles"
diff -qr /Library/GPUBundles "$snapshot/GPUBundles"
diff -qr "$root/private-repo" "$snapshot/DriverPackage/private-repo"
for file in esp.sh onboot-stable.sh onboot-verified.sh commit-next-boot.sh restore-efi-only.sh health.sh trial-config.plist Arrow-Native-candidate.kc; do
    cmp -s "$root/$file" "$snapshot/DriverPackage/$file"
done
mkdir "$snapshot/KernelCollections"
cp /Library/KernelCollections/AuxiliaryKernelExtensions.kc "$snapshot/KernelCollections/AuxiliaryKernelExtensions.kc"
cmp -s /Library/KernelCollections/AuxiliaryKernelExtensions.kc "$snapshot/KernelCollections/AuxiliaryKernelExtensions.kc"
shopt -s nullglob
for dir in /Library/LaunchDaemons /Library/LaunchAgents /Users/hackintosh/Library/LaunchAgents; do
    for file in "$dir"/*.plist; do
        case "$(basename "$file")" in
            *wtl*|*WTL*|*arrow*|*Arrow*)
                dest="$snapshot/Launchd$file"
                mkdir -p "$(dirname "$dest")"
                cp -p "$file" "$dest"
                cmp -s "$file" "$dest"
                ;;
        esac
    done
done
mkdir "$snapshot/Management"
for file in config.debug.private.plist config.quiet.private.plist toggle-debug.sh README.md manifest.json; do
    cp "$pkg/$file" "$snapshot/Management/$file"
done
sw_vers > "$snapshot/system-version.txt"
sysctl -n kern.bootsessionuuid > "$snapshot/snapshot-boot-uuid.txt"
kmutil showloaded --list-only > "$snapshot/loaded-kexts.txt" 2>&1
"$root/CPUTemperature.1/smcread" -s | awk '/^\[TC/' > "$snapshot/temperature-keys.txt"
bash "$root/health.sh" > "$snapshot/graphics-health.txt"
shasum -a 256 -c "$pkg/protected-before.sha256"
[ "$(hash "$root/trial-config.plist")" = "$new" ]
cmp -s "$root/trial-config.plist" "$snapshot/EFI/OC/config.plist"
cmp -s "$root/trial-config.plist" "$snapshot/DriverPackage/trial-config.plist"
echo PHASE_HASH_AND_VERIFY_SNAPSHOT
(
    cd "$snapshot"
    find . -type f ! -name SHA256SUMS -print0 | xargs -0 shasum -a 256 > SHA256SUMS
    shasum -a 256 -c SHA256SUMS > "$pkg/snapshot-check.log"
)
echo PHASE_CREATE_PRIVATE_STABLE_ARCHIVE
archive="$pkg/$id.tar.gz"
tar -czf "$archive" -C "$snapshot" .
gzip -t "$archive"
shasum -a 256 "$archive" > "$pkg/archive.sha256"
chown hackintosh:staff "$archive"
chmod 600 "$archive"
chmod 644 "$pkg/archive.sha256" "$pkg/installed.sha256"
cat > "$pkg/stable-state.json" <<EOF
{"stable_id":"$id","installed":true,"quiet_profile_sha256":"$new","source_profile_sha256":"$old","snapshot_verified":true,"quiet_logo_next_boot_validated":false,"restart_performed":false,"archive":"$id.tar.gz"}
EOF
chmod 644 "$pkg/stable-state.json"
sync
touch "$pkg/STABLE_SAVED_QUIET_NEXT_BOOT_NO_RESTART"
chmod 644 "$pkg/STABLE_SAVED_QUIET_NEXT_BOOT_NO_RESTART"
completed=1
trap - EXIT
echo STABLE_SAVED_QUIET_NEXT_BOOT_NO_RESTART
