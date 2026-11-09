#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
[ "$(id -u)" = 0 ]
[ "$(uname -m)" = x86_64 ]
[ "$(sysctl -n kern.bootsessionuuid)" = 65AD1A65-B16F-5A3F-B973-F49FE57BDA70 ]
root='/Library/Application Support/WTLArrowTiming.EXAMPLE'
stage=$(cd "$(dirname "$0")" && pwd)
payload="$stage/payload"
[ ! -e "$root/HiDPIRepair.2" ]
for f in validate-display.sh probe-display.sh switch-desktop.sh LiveDisplayAudit.app; do [ ! -e "$root/$f" ]; done
(cd "$payload" && shasum -c "$stage/payload.sha256")
(cd "$root" && shasum -c "$stage/before-root.sha256")
(cd / && shasum -c "$stage/accepted-artifacts-before.sha256")
for f in trial-config.plist stable-config.plist config.quiet.private.plist BootstrapRebase.1/bootstrap.private.plist; do
    "$payload/ocvalidate" "$payload/$f"
    [ "$(plutil -extract Misc.Boot.Timeout raw -o - "$payload/$f")" = 5 ]
    [ "$(plutil -extract ACPI.Quirks.RebaseRegions raw -o - "$payload/$f")" = true ]
done
. "$root/esp.sh"
mount_esp
trap unmount_esp EXIT
[ "$(shasum -a 256 "$esp_config" | awk '{print $1}')" = 09409ec0484908eaf5f00d16d4538ee158bb41a633696483fbf13217c88ed923 ]
(cd "$esp_mp/EFI/OC" && shasum -c "$stage/enabled-files.sha256")
pkg="$root/HiDPIRepair.2"
mkdir -m 755 "$pkg"
mkdir -m 700 "$pkg/before"
before="$pkg/before"
ditto --norsrc "$esp_mp/EFI" "$before/EFI"
cp -p /Library/KernelCollections/AuxiliaryKernelExtensions.kc "$before/AuxiliaryKernelExtensions.kc"
mkdir "$before/root"
while IFS= read -r f; do
    mkdir -p "$before/root/$(dirname "$f")"
    cp -p "$root/$f" "$before/root/$f"
done < "$stage/changed-root-files.txt"
ditto --norsrc "$root/BootstrapRebase.1" "$before/BootstrapRebase.1"
(cd "$esp_mp/EFI" && find . -type f ! -path './OC/config.plist' ! -path './OC/._config.plist' -exec shasum -a 256 '{}' + | sort) > "$pkg/unchanged-efi.sha256"
ditto --norsrc "$payload" "$pkg/payload"
cp "$stage/changed-root-files.txt" "$pkg/changed-root-files.txt"
cp "$stage/rollback.sh" "$pkg/rollback.sh"
chmod 744 "$pkg/rollback.sh"
completed=0
changed=0
finish() {
    rc=$?
    trap - EXIT
    set +e
    if [ "$completed" = 0 ] && [ "$changed" = 1 ]; then
        bash "$pkg/rollback.sh"
        echo INSTALL_FAILED_PRIOR_FILES_RESTORED
    fi
    unmount_esp
    exit "$rc"
}
trap finish EXIT
changed=1
while IFS= read -r f; do
    cp "$payload/$f" "$root/$f.repair-new"
    chown root:wheel "$root/$f.repair-new"
    chmod 644 "$root/$f.repair-new"
    case "$f" in *.sh) chmod 744 "$root/$f.repair-new";; esac
    mv -f "$root/$f.repair-new" "$root/$f"
done < "$stage/changed-root-files.txt"
for f in validate-display.sh probe-display.sh switch-desktop.sh; do
    cp "$payload/$f" "$root/$f"
    chown root:wheel "$root/$f"
    chmod 744 "$root/$f"
done
ditto --norsrc "$payload/LiveDisplayAudit.app" "$root/LiveDisplayAudit.app"
chown -R root:wheel "$root/LiveDisplayAudit.app"
codesign --verify --deep --strict "$root/LiveDisplayAudit.app"
rm -f "$root/BootstrapRebase.1/CONSUMED"
printf 'HIDPI_REPAIR_NEXT_BOOT_5_SECONDS\n' > "$root/BootstrapRebase.1/PENDING"
chown -R root:wheel "$root/BootstrapRebase.1"
chmod 755 "$root/BootstrapRebase.1"
# Verify the exact installed reader can reach the logged-in GUI. This boot is
# deliberately still the known 1080p fallback, so the HiDPI validator must reject it.
probe_rc=0
bash "$root/probe-display.sh" audit > "$pkg/current-basic-display-check.log" 2>&1 || probe_rc=$?
[ "$probe_rc" = 1 ]
request=$(ls -td "$root"/live-display-audits/request.* | head -1)
[ "$(plutil -extract boot_uuid raw -o - "$request/result.json")" = 65AD1A65-B16F-5A3F-B973-F49FE57BDA70 ]
[ "$(plutil -extract current.pixel_width raw -o - "$request/result.json")" = 1920 ]
[ "$(plutil -extract current.pixel_height raw -o - "$request/result.json")" = 1080 ]
[ "$(plutil -extract online raw -o - "$request/result.json")" = true ]
echo CURRENT_1080P_REJECTED_BY_FRESH_HIDPI_CHECK > "$pkg/current-basic-display-check.txt"
management='/Library/Application Support/WTLStable/WTL-265K-HiDPI-20261109-v5'
[ ! -e "$management" ]
mkdir -m 755 "$management"
cp "$payload/trial-config.plist" "$management/config.debug.private.plist"
cp "$payload/config.quiet.private.plist" "$management/config.quiet.private.plist"
cp "$payload/stable-config.plist" "$management/config.fallback.private.plist"
cp "$payload/toggle-debug.sh" "$management/toggle-debug.sh"
chmod 744 "$management/toggle-debug.sh"
printf '5-second native and safe-fallback profiles. Historical v1-v3 backups retained.\n' > "$management/README.txt"
write_collection "$root/Arrow-Native-candidate.kc"
write_config "$root/BootstrapRebase.1/bootstrap.private.plist"
[ "$(plutil -extract Misc.Boot.Timeout raw -o - "$esp_config")" = 5 ]
(cd "$esp_mp/EFI" && shasum -c "$pkg/unchanged-efi.sha256")
(cd / && shasum -c "$stage/accepted-artifacts-after.sha256")
while IFS= read -r f; do cmp -s "$payload/$f" "$root/$f"; done < "$stage/changed-root-files.txt"
for f in validate-display.sh probe-display.sh switch-desktop.sh; do cmp -s "$payload/$f" "$root/$f"; done
cmp -s "$root/BootstrapRebase.1/bootstrap.private.plist" "$esp_config"
shasum -a 256 "$esp_config" "$root/trial-config.plist" "$root/stable-config.plist" /Library/KernelCollections/AuxiliaryKernelExtensions.kc > "$pkg/installed.sha256"
printf 'PREPARED_NOT_RUNTIME_VERIFIED\n' > "$pkg/RESULT"
tar -cf "$pkg/before.tar" -C "$pkg" before unchanged-efi.sha256 changed-root-files.txt rollback.sh
chown hackintosh:staff "$pkg/before.tar"
chmod 600 "$pkg/before.tar"
if [ -d "$root/HiDPIRepair.1/before" ]; then
    tar -cf "$root/HiDPIRepair.1/before.tar" -C "$root/HiDPIRepair.1" before unchanged-efi.sha256 changed-root-files.txt rollback.sh
    chown hackintosh:staff "$root/HiDPIRepair.1/before.tar"
    chmod 600 "$root/HiDPIRepair.1/before.tar"
fi
sync
completed=1
unmount_esp
trap - EXIT
echo BACKUP_AND_HIDPI_REPAIR_INSTALLED_NEXT_BOOT_TIMEOUT_5_NO_RESTART
