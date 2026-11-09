#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
[ "$(id -u)" = 0 ]
[ "$(sysctl -n kern.bootsessionuuid)" = F947797D-DFC5-524F-AF66-8C37D7B2FA9F ]
stage=$(cd "$(dirname "$0")" && pwd)
root='/Library/Application Support/WTLArrowTiming.EXAMPLE'
restore="$root/WiFiD3Restore.1"
cd "$stage"
shasum -a 256 --strict --quiet -c SHA256SUMS
./ocvalidate restore-config.plist
[ "$(shasum -a 256 "$root/NativePowerTrial.1/AirportRTW.kext/Contents/MacOS/AirportRTW" | awk '{print $1}')" = 04ff7098bdb66e66d679e117b8ce86609d72d3a89322380614c777aa360a825b ]
codesign --verify --deep --strict "$root/NativePowerTrial.1/AirportRTW.kext"
[ "$(shasum -a 256 "$root/Arrow-Native-candidate.kc" | awk '{print $1}')" = 910da863fd31d575d792fdd3f2bb2bf59c295010fe2cc2a584e6ceffaf52fa27 ]
[ "$(shasum -a 256 "$root/XeMTLDriver.bundle/Contents/MacOS/XeMTLDriver" | awk '{print $1}')" = 6f2e9c5b6306ee132ccca967e911a42d904c85f5f94917f6b65066fd007de696 ]
codesign --verify --deep --strict "$root/XeMTLDriver.bundle"
[ -s "$root/XeMTLDriver.bundle/Contents/MacOS/air-runtime/libLLVM.dylib" ]
[ -s "$root/XeMTLDriver.bundle/Contents/MacOS/air-runtime/libzstd.1.dylib" ]
[ "$(shasum -a 256 "$root/onboot-verified.sh" | awk '{print $1}')" = 611870bc8466c8350a6199c9a64b84e933a59d9315369ecc98b1b74c9817fde9 ]
[ ! -e "$restore" ]
mkdir -m 755 "$restore"
ditto --norsrc "$stage" "$restore"
chown -R root:wheel "$restore"
cp "$root/trial-config.plist" "$restore/graphics-profile-before.plist"
cmp "$restore/graphics-profile-before.plist" "$stage/graphics-profile-before.plist"
cp "$root/restore-efi-only.sh" "$restore/restore-helper-before.sh"
cp /Library/KernelCollections/AuxiliaryKernelExtensions.kc "$restore/Aux-before.kc"
cp "$root/onboot-verified.sh" "$restore/onboot-verified.before.sh"
for name in XeMTLDriver.bundle xemtl; do
    if [ -e "/Library/GPUBundles/$name" ]; then
        ditto --norsrc "/Library/GPUBundles/$name" "$restore/userland-before-$name"
    else
        touch "$restore/userland-before-$name.was-absent"
    fi
done
for name in xemtl-allow.txt .xemtl-fbfetch-ws; do
    if [ -e "/Library/GPUBundles/$name" ]; then cp "/Library/GPUBundles/$name" "$restore/userland-before-$name"; else touch "$restore/userland-before-$name.was-absent"; fi
done
tail -40 "$root/stable-startup.log" > "$restore/startup-before.txt" 2>&1 || true
tail -60 "$root/result.log" >> "$restore/startup-before.txt" 2>&1 || true
[ ! -f "$root/RESULT" ] || cat "$root/RESULT" >> "$restore/startup-before.txt"
cat "$restore/startup-before.txt"
# Pause only this boot's waiting coordinator, avoiding its failure trap racing
# the restore. The job/plist is unchanged and runs normally after the next boot.
ps -axo pid,command | awk '/\/WTLArrowTiming[.]li8Pgu\/onboot-(stable|verified)[.]sh/ {print $1}' > "$restore/paused-startup-pids.txt"
early_cleanup() {
    rc=$?
    trap - EXIT
    set +e
    if type unmount_esp >/dev/null 2>&1; then unmount_esp; fi
    while read -r pid; do [ -z "$pid" ] || kill -CONT "$pid" 2>/dev/null; done < "$restore/paused-startup-pids.txt"
    exit "$rc"
}
trap early_cleanup EXIT
while read -r pid; do
    [ -n "$pid" ] || continue
    if ps -p "$pid" -o command= | grep -E '/WTLArrowTiming[.]li8Pgu/onboot-(stable|verified)[.]sh' >/dev/null; then
        kill -STOP "$pid"
    fi
done < "$restore/paused-startup-pids.txt"
[ -f /Library/LaunchDaemons/org.local.wtl-arrow-native-stable.plist ]
. "$root/esp.sh"
mount_esp
cp "$esp_config" "$restore/efi-before.plist"
if ! cmp -s "$esp_config" "$root/stable-config.plist" && ! cmp -s "$esp_config" "$root/trial-config.plist"; then
    cmp "$esp_config" "$root/NativeBATrial.1/config.wifi.plist"
fi
for key in xegfx-disp-once xegfx-disp-phase xegfx-disp-trial xegfx-disp-fail xegfx-disp-external-dp xegfx-disp-kms xegfx-disp-bootarm xegfx-disp-expose xegfx-disp-starts; do
    if nvram -x "$key" > "$restore/$key.before.plist" 2>/dev/null; then
        plutil -lint "$restore/$key.before.plist" >/dev/null
    else
        rm -f "$restore/$key.before.plist"
        touch "$restore/$key.was-absent"
    fi
done
changed=0
completed=0
finish() {
    rc=$?
    trap - EXIT
    set +e
    if [ "$changed" = 1 ] && [ "$completed" = 0 ]; then
        cp "$restore/graphics-profile-before.plist" "$root/trial-config.plist"
        cp "$restore/restore-helper-before.sh" "$root/restore-efi-only.sh"
        chown root:wheel "$root/trial-config.plist" "$root/restore-efi-only.sh"
        chmod 755 "$root/restore-efi-only.sh"
        write_config "$restore/efi-before.plist"
        write_collection "$restore/Aux-before.kc"
        cp "$restore/onboot-verified.before.sh" "$root/onboot-verified.sh"
        chown root:wheel "$root/onboot-verified.sh"
        chmod 744 "$root/onboot-verified.sh"
        for name in XeMTLDriver.bundle xemtl; do
            rm -rf "/Library/GPUBundles/$name"
            [ ! -e "$restore/userland-before-$name" ] || ditto --norsrc "$restore/userland-before-$name" "/Library/GPUBundles/$name"
        done
        for name in xemtl-allow.txt .xemtl-fbfetch-ws; do
            rm -f "/Library/GPUBundles/$name"
            [ ! -e "$restore/userland-before-$name" ] || cp "$restore/userland-before-$name" "/Library/GPUBundles/$name"
        done
        for key in xegfx-disp-once xegfx-disp-phase xegfx-disp-trial xegfx-disp-fail xegfx-disp-external-dp xegfx-disp-kms xegfx-disp-bootarm xegfx-disp-expose xegfx-disp-starts; do
            if [ -f "$restore/$key.before.plist" ]; then nvram -x -f "$restore/$key.before.plist"; else nvram -d "$key" >/dev/null 2>&1; fi
        done
        echo RESTORE_PREPARATION_FAILED_PREVIOUS_PROFILE_RESTORED_NO_RESTART
    fi
    if [ "$completed" = 0 ]; then
        while read -r pid; do [ -z "$pid" ] || kill -CONT "$pid" 2>/dev/null; done < "$restore/paused-startup-pids.txt"
    fi
    unmount_esp
    exit "$rc"
}
trap finish EXIT
dest="$esp_mp/EFI/OC/Kexts/WTLNativePowerTrial1"
free_kb=$(df -k "$esp_mp" | tail -1 | awk '{print $4}')
need_kb=$(du -sk "$root/NativePowerTrial.1/AirportRTW.kext" | awk '{print $1}')
[ "$free_kb" -ge "$((need_kb + 8192))" ]
if [ -d "$dest" ]; then
    diff -qr -x '._*' "$root/NativePowerTrial.1/AirportRTW.kext" "$dest/AirportRTW.kext"
else
    mkdir "$dest"
    ditto --norsrc "$root/NativePowerTrial.1/AirportRTW.kext" "$dest/AirportRTW.kext"
    diff -qr -x '._*' "$root/NativePowerTrial.1/AirportRTW.kext" "$dest/AirportRTW.kext"
fi
original_helper="$root/NativePowerTrial.1/restore-efi-only.before.sh"
[ "$(shasum -a 256 "$original_helper" | awk '{print $1}')" = 2fab6bafd51f2a87b1f30d95089691d2407a67c5c1d369ecf219ed67568d1db5 ]
changed=1
mkdir -p /Library/GPUBundles
for name in XeMTLDriver.bundle xemtl; do
    ditto --norsrc "$root/$name" "/Library/GPUBundles/$name.d3-restore-new"
    rm -rf "/Library/GPUBundles/$name"
    mv "/Library/GPUBundles/$name.d3-restore-new" "/Library/GPUBundles/$name"
    chown -R root:wheel "/Library/GPUBundles/$name"
    chmod -R go-w "/Library/GPUBundles/$name"
done
[ "$(shasum -a 256 /Library/GPUBundles/XeMTLDriver.bundle/Contents/MacOS/XeMTLDriver | awk '{print $1}')" = 6f2e9c5b6306ee132ccca967e911a42d904c85f5f94917f6b65066fd007de696 ]
codesign --verify --deep --strict /Library/GPUBundles/XeMTLDriver.bundle
printf '# Probe first; original startup checks enable WindowServer after readiness\nmtlprobe\n' > /Library/GPUBundles/xemtl-allow.txt
touch /Library/GPUBundles/.xemtl-fbfetch-ws
chown root:wheel /Library/GPUBundles/xemtl-allow.txt /Library/GPUBundles/.xemtl-fbfetch-ws
chmod 644 /Library/GPUBundles/xemtl-allow.txt /Library/GPUBundles/.xemtl-fbfetch-ws
cp "$restore/onboot-verified-timing-wait.sh" "$root/onboot-verified.sh.restore-new"
chown root:wheel "$root/onboot-verified.sh.restore-new"
chmod 744 "$root/onboot-verified.sh.restore-new"
mv -f "$root/onboot-verified.sh.restore-new" "$root/onboot-verified.sh"
[ "$(shasum -a 256 "$root/onboot-verified.sh" | awk '{print $1}')" = 73ceb7ebb2769a9d209b362a32c99b1ab8bd7de6f4892ba83946ba2d0946a718 ]
cp "$original_helper" "$root/restore-efi-only.sh.restore-new"
chown root:wheel "$root/restore-efi-only.sh.restore-new"
chmod 755 "$root/restore-efi-only.sh.restore-new"
mv -f "$root/restore-efi-only.sh.restore-new" "$root/restore-efi-only.sh"
# The normal graphics success path will re-arm this same d3 profile after login.
# The original graphics-only stable-config remains the recovery entry.
cp "$restore/restore-config.plist" "$root/trial-config.plist.restore-new"
chown root:wheel "$root/trial-config.plist.restore-new"
chmod 644 "$root/trial-config.plist.restore-new"
mv -f "$root/trial-config.plist.restore-new" "$root/trial-config.plist"
write_collection "$root/Arrow-Native-candidate.kc"
nvram -x -f "$restore/graphics-gates.plist"
"$root/gate-audit" > "$restore/rearmed-gates.txt"
write_config "$root/trial-config.plist"
cmp "$root/trial-config.plist" "$esp_config"
cmp "$root/Arrow-Native-candidate.kc" /Library/KernelCollections/AuxiliaryKernelExtensions.kc
sysctl -n kern.bootsessionuuid > "$root/prepared-boot"
cp "$root/prepared-boot" "$restore/prepared-boot"
cp "$restore/collector.plist" /Library/LaunchDaemons/org.local.wtl-wifi-d3-restore-once.plist
chown root:wheel /Library/LaunchDaemons/org.local.wtl-wifi-d3-restore-once.plist
chmod 644 /Library/LaunchDaemons/org.local.wtl-wifi-d3-restore-once.plist
shasum -a 256 "$esp_config" "$root/trial-config.plist" "$root/restore-efi-only.sh" /Library/KernelCollections/AuxiliaryKernelExtensions.kc "$dest/AirportRTW.kext/Contents/MacOS/AirportRTW" > "$restore/enabled.sha256"
cat "$restore/enabled.sha256" "$restore/rearmed-gates.txt"
sync
completed=1
unmount_esp
trap - EXIT
echo VERIFIED_D3_AND_4K_RESTORE_PREPARED_NO_RESTART
