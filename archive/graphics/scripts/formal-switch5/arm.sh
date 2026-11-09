#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
test "$(id -u)" = 0
root=$(cd "$(dirname "$0")" && pwd)
test -f "$root/PREFLIGHT_READY"
test -s "$root/explicit-restart-consent.txt"
test "$(sysctl -n kern.bootsessionuuid)" = "$(cat "$root/prepared-boot")"
job=/Library/LaunchDaemons/org.local.wtl-arrow-native5-once.plist
test ! -e "$job"
. "$root/esp.sh"
committed=0
cleanup() {
    rc=$?
    trap - EXIT
    set +e
    if [ "$committed" = 0 ]; then
        bash "$root/restore.sh"
        [ ! -f "$job" ] || mv "$job" "$root/aborted-job.plist"
        echo ARM_FAILED_ORIGINAL_CONFIGURATION_RESTORED
    fi
    exit "$rc"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
mkdir -p /Library/GPUBundles
touch "$root/userland-installed"
for name in XeMTLDriver.bundle xemtl; do
    ditto --norsrc "$root/$name" "/Library/GPUBundles/$name.arrow-new"
    rm -rf "/Library/GPUBundles/$name"
    mv "/Library/GPUBundles/$name.arrow-new" "/Library/GPUBundles/$name"
    chown -R root:wheel "/Library/GPUBundles/$name"
    chmod -R go-w "/Library/GPUBundles/$name"
done
printf '# Probe first; WindowServer enabled only after the native pipe and Metal probe pass\nmtlprobe\n' > "/Library/GPUBundles/xemtl-allow.txt"
touch "/Library/GPUBundles/.xemtl-fbfetch-ws"
chown root:wheel "/Library/GPUBundles/xemtl-allow.txt" "/Library/GPUBundles/.xemtl-fbfetch-ws"
chmod 644 "/Library/GPUBundles/xemtl-allow.txt" "/Library/GPUBundles/.xemtl-fbfetch-ws"
nvram -x -f "$root/gates.plist"
"$root/gate-audit" > "$root/armed-gate-audit.txt"
cat "$root/armed-gate-audit.txt"
nvram xegfx-disp-once | grep -q '%01%00%00%00'
cat > "$job" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>Label</key><string>org.local.wtl-arrow-native5-once</string>
<key>ProgramArguments</key><array><string>/bin/bash</string><string>$root/onboot.sh</string></array>
<key>RunAtLoad</key><true/>
<key>StandardOutPath</key><string>$root/job.log</string>
<key>StandardErrorPath</key><string>$root/job.log</string>
</dict></plist>
PLIST
chown root:wheel "$job"
chmod 644 "$job"
plutil -lint "$job"
write_collection "$root/Arrow-Native-candidate.kc"
mount_esp
cmp -s "$root/stable-config.plist" "$esp_config"
test ! -e "$esp_mp/EFI/OC/Kexts/WTLArrowTrial3"
mkdir "$esp_mp/EFI/OC/Kexts/WTLArrowTrial3"
ditto --norsrc "$root/WTLArrowEarlyClaim.kext" "$esp_mp/EFI/OC/Kexts/WTLArrowTrial3/WTLArrowEarlyClaim.kext"
cmp -s "$root/WTLArrowEarlyClaim.kext/Contents/MacOS/WTLArrowEarlyClaim" "$esp_mp/EFI/OC/Kexts/WTLArrowTrial3/WTLArrowEarlyClaim.kext/Contents/MacOS/WTLArrowEarlyClaim"
cmp -s "$root/WTLArrowEarlyClaim.kext/Contents/Info.plist" "$esp_mp/EFI/OC/Kexts/WTLArrowTrial3/WTLArrowEarlyClaim.kext/Contents/Info.plist"
write_config "$root/trial-config.plist"
cmp -s "$root/trial-config.plist" "$esp_config"
printf 'efi_partition=%s\nconfig_path=%s\n' "$esp_part" "$esp_config" > "$root/enabled-efi.txt"
shasum -a 256 "$esp_config" "$esp_mp/EFI/OC/Kexts/WTLArrowTrial3/WTLArrowEarlyClaim.kext/Contents/MacOS/WTLArrowEarlyClaim" > "$root/enabled-efi.sha256"
unmount_esp
cmp -s "$root/Arrow-Native-candidate.kc" /Library/KernelCollections/AuxiliaryKernelExtensions.kc
kmutil inspect --show-kext-uuids -A /Library/KernelCollections/AuxiliaryKernelExtensions.kc > "$root/enabled-cache-uuids.txt"
while read -r ident version uuid sha; do
    if [ "$ident" = org.local.WTLArrowEarlyClaim ]; then continue; fi
    grep -F "$ident" "$root/enabled-cache-uuids.txt" | grep -Fq "$uuid"
done < "$root/expected-modules.txt"
printf 'authorized_additional_manual_restarts=1\nautomatically_initiated_restarts=0\n' > "$root/restart-budget.txt"
sync
touch "$root/READY_TO_MANUALLY_RESTART"
committed=1
trap - EXIT HUP INT TERM
echo NEW_EFI_NEW_DRIVER_IDENTITIES_VERIFIED_READY_FOR_ONE_MANUAL_RESTART
printf '\n配置和驱动已按唯一标识核对通过，现在可以手动重启黑苹果一次。\n'
