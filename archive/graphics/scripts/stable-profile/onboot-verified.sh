#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
test "$(id -u)" = 0
root=$(cd "$(dirname "$0")" && pwd)
exec >> "$root/result.log" 2>&1
job=/Library/LaunchDaemons/org.local.wtl-arrow-native5-once.plist
test "$(sysctl -n kern.bootsessionuuid)" != "$(cat "$root/prepared-boot")"
if [ -d "$root/started" ]; then
    bash "$root/restore.sh"
    [ ! -f "$job" ] || mv "$job" "$root/recovery-job.plist"
    echo RECOVERY_BOOT_ORIGINAL_CONFIGURATION_RESTORED
    exit 0
fi
mkdir "$root/started"
sysctl -n kern.bootsessionuuid > "$root/tested-boot"
finish() {
    rc=$?
    trap - EXIT
    set +e
    sysctl -b debug.xegfx_disp > "$root/display-report.txt"
    sysctl -b debug.xegfx_log > "$root/core-runtime.log"
    sysctl -a 2>/dev/null | grep -E '^debug\.(xegfx|nmxaccel)' > "$root/sysctl-final.txt"
    kmutil showloaded --list-only > "$root/loaded-kexts.txt"
    ioreg -a -l -r -c IOFramebuffer > "$root/framebuffers.plist"
    ioreg -a -l -r -c NMXAccel > "$root/accelerator.plist"
    [ ! -f "$job" ] || mv "$job" "$root/consumed-job.plist"
    if [ "$rc" != 0 ]; then
        bash "$root/restore.sh"
        echo "FAILED_AT_${current_stage:-unknown}_NEXT_BOOT_ORIGINAL_RESTORED" > "$root/RESULT"
    fi
    echo "NATIVE_TRIAL_EXIT=$rc"
    exit "$rc"
}
trap finish EXIT
current_stage=verify_boot_identity
"$root/gate-audit" --consumed > "$root/boot-gate-audit.txt" 2>&1 || true
log show --last 4m --style compact --predicate 'process == "kernel" AND eventMessage CONTAINS "WTLArrowEarlyClaim"' > "$root/early-claim-gate-log.txt" 2>&1 || true

bash "$root/restore-efi-only.sh"
sysctl -n kern.bootargs > "$root/actual-bootargs.txt"
grep -q 'wtl-arrow-native=3' "$root/actual-bootargs.txt"
grep -q 'wtl-arrow-timing=45' "$root/actual-bootargs.txt"
nvram '4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:boot-path' > "$root/actual-opencore-path.txt"
grep -iq '00000000-0000-0000-0000-000000000001' "$root/actual-opencore-path.txt"
ioreg -l -w0 -r -c WTLArrowEarlyClaim > "$root/early-claim.txt"
grep -q '"WTLArrowEarlyClaimActive" = Yes' "$root/early-claim.txt"
nvram xegfx-disp-once | grep -q '%00%00%00%00'
ioreg -l -w0 -r -c IOFramebuffer > "$root/framebuffers-before-loader.txt"
if grep -q 'class IONDRVFramebuffer' "$root/framebuffers-before-loader.txt"; then echo STOCK_FRAMEBUFFER_ALREADY_OWNS_DISPLAY; exit 1; fi
verify_loaded() {
    local ident=$1 uuid
    uuid=$(awk -v key="$ident" '$1==key {print $3}' "$root/expected-modules.txt")
    test -n "$uuid"
    kmutil showloaded --list-only -b "$ident" > "$root/loaded-$ident.txt"
    grep -Fq "$uuid" "$root/loaded-$ident.txt"
}
verify_loaded org.local.WTLArrowEarlyClaim
kmutil showloaded --show all --list-only --collection aux > "$root/actual-aux-cache.txt"
while read -r ident version uuid sha; do
    if [ "$ident" = org.local.WTLArrowEarlyClaim ]; then continue; fi
    grep -F "$ident (" "$root/actual-aux-cache.txt" | grep -Fq "$uuid"
done < "$root/expected-modules.txt"
touch "$root/NEW_CONFIGURATION_AND_EARLY_DRIVER_CONFIRMED"
current_stage=explicit_driver_load
for name in XeGfxAGDC XeGfxDisplay XeGfx; do
    kmutil load --no-authorization -p "$root/private-repo/$name.kext"
    verify_loaded "org.watchthelight.$name"
done
current_stage=gpu_initialization
core_ready=0
for step in $(seq 1 60); do
    if [ "$(sysctl -n debug.xegfx_stage 2>/dev/null || echo 0)" = 7 ]; then core_ready=1; break; fi
    sleep 2
done
test "$core_ready" = 1
kmutil load --no-authorization -p "$root/private-repo/XeGfxAccel.kext"
verify_loaded org.watchthelight.XeGfxAccel
touch "$root/NEW_CORE_AND_DISPLAY_MODULES_CONFIRMED"
current_stage=native_pipe
ready=0
for step in $(seq 1 120); do
    stage=$(sysctl -n debug.xegfx_stage 2>/dev/null || echo 0)
    accel=$(sysctl -n debug.xegfxaccel_boot 2>/dev/null || echo 0)
    if [ "$stage" = 7 ] && [ "$accel" = 2 ]; then ready=1; break; fi
    sleep 2
done
test "$ready" = 1
. "$root/esp.sh"
write_collection "$root/AuxiliaryKernelExtensions-before.kc"
echo ALL_NEW_MODULES_LOADED_NEXT_BOOT_ORIGINAL_CACHE_RESTORED
ioreg -l -w0 -r -c XeGfxIntelKMSFramebuffer > "$root/native-framebuffer.txt"
grep -q 'class XeGfxIntelKMSFramebuffer' "$root/native-framebuffer.txt"
grep -q '"XeGfxFbRegistered" = Yes' "$root/native-framebuffer.txt"
test "$(sysctl -n debug.xegfxaccel_heads_registered)" = 16
test "$(sysctl -n debug.xegfxaccel_heads_piped)" = 16
sysctl -b debug.xegfx_disp > "$root/display-report.txt"
grep -E 'commit.*(ok|ret 0|ret=0)|K3 fixed mode .*3840x2160.*committed' "$root/display-report.txt"
current_stage=published_timing
"$root/agdc-timing-check" > "$root/timing-check.txt" 2>&1
grep -q 'live NVRMTiming: present' "$root/timing-check.txt"
current_stage=alias_probe
NM_TEST_PLUGIN=/Library/GPUBundles/XeMTLDriver.bundle/Contents/MacOS/XeMTLDriver XEMTL_LOG_PATH="$root/alias-probe-plugin.log" "$root/alias/mtlprobe" > "$root/alias-probe-result.txt" 2>&1
grep -q 'pooled alias GPU clear and CPU visibility: OK' "$root/alias-probe-result.txt"
grep -q 'dedicated alias GPU clear and CPU visibility: OK' "$root/alias-probe-result.txt"
current_stage=metal_probe
NM_TEST_PLUGIN=/Library/GPUBundles/XeMTLDriver.bundle/Contents/MacOS/XeMTLDriver XEMTL_LOG_PATH="$root/mtlprobe-plugin.log" "$root/mtlprobe" > "$root/mtlprobe-result.txt" 2>&1
grep -q 'metal compute: OK' "$root/mtlprobe-result.txt"
grep -q 'metal draw: OK' "$root/mtlprobe-result.txt"
touch "$root/METAL_PROBE_PASSED"
printf '# Native DP-H trial\n!WindowServer\nmtlprobe\nDisplayApply\n' > "/Library/GPUBundles/xemtl-allow.txt"
chown root:wheel "/Library/GPUBundles/xemtl-allow.txt"
chmod 644 "/Library/GPUBundles/xemtl-allow.txt"
current_stage=windowserver_switch
before=$(pgrep -x WindowServer || true)
printf '%s\n' "$before" > "$root/windowserver-before.txt"
launchctl kickstart -k system/com.apple.WindowServer > "$root/windowserver-kickstart.txt" 2>&1
touch "$root/COMPOSITOR_SWITCHED"
sleep 10
sysctl -a 2>/dev/null | grep -E '^debug\.(xegfx_agdc|xegfxaccel_iop)' > "$root/capabilities-after-switch.txt"
ws=$(pgrep -x WindowServer | head -1)
log show --last 2m --style compact --predicate "processID == $ws AND messageType == error" > "$root/windowserver-early-errors.txt" 2>&1 || true
for step in $(seq 1 300); do
    if [ "$(stat -f '%Su' /dev/console)" = hackintosh ]; then break; fi
    sleep 2
done
if [ "$(stat -f '%Su' /dev/console)" = hackintosh ]; then
    launchctl asuser 501 sudo -u hackintosh /usr/bin/open -W -n "$root/DisplayApply.app" --args --select-hidpi > "$root/hidpi-apply.log" 2>&1 || true
    cp /Users/hackintosh/Library/Caches/WTLArrowNative/display-audit.json "$root/display-audit.json" || true
fi
sleep 15
ws=$(pgrep -x WindowServer | head -1)
printf '%s\n' "$ws" > "$root/windowserver-after.txt"
sample "$ws" 1 20 -file "$root/windowserver.sample.txt" >/dev/null 2>&1 || true
log show --last 5m --style compact --predicate "processID == $ws AND eventMessage CONTAINS[c] 'framebuffer fetch'" > "$root/windowserver-fetch.log" 2>&1 || true
current_stage=review_ready
echo NATIVE_COMPOSITOR_STARTED_PENDING_VISUAL_AND_HIDPI_ACCEPTANCE > "$root/RESULT"
touch "$root/READY_FOR_REVIEW"
