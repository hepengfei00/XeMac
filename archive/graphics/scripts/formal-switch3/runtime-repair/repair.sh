#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
test "$(id -u)" = 0
test "${SUDO_USER:-}" = hackintosh
test "$(sysctl -n kern.bootsessionuuid)" = 8DA5A522-CBA1-5582-9441-875DBA1BA3FB
test "$(sysctl -n debug.xegfx_stage)" = 7
test "$(sysctl -n debug.xegfxaccel_boot)" = 2
root='/Library/Application Support/WTLArrowNative2.U8gnc8'
src=$(cd "$(dirname "$0")" && pwd)
repair=$(mktemp -d '/Library/Application Support/WTLArrowRuntime.XXXXXX')
chmod 755 "$repair"
printf '%s\n' "$repair" > "$src/root-workdir.txt"
exec > >(tee -a "$repair/repair.log") 2>&1
finish() {
    rc=$?
    trap - EXIT
    set +e
    sysctl -b debug.xegfx_disp > "$repair/display-report.txt"
    sysctl -a 2>/dev/null | grep -E '^debug\.(xegfx|nmxaccel)' > "$repair/sysctl-final.txt"
    ioreg -l -w0 -r -c XeGfxIntelKMSFramebuffer > "$repair/framebuffer.txt"
    kmutil showloaded --list-only > "$repair/loaded-kexts.txt"
    echo "RUNTIME_REPAIR_EXIT=$rc; NO_MACHINE_REBOOT_REQUESTED"
    exit "$rc"
}
trap finish EXIT
runtime=/Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS/air-runtime
test -s "$runtime/libLLVM.dylib"
test -s "$runtime/libzstd.1.dylib"
ditto --norsrc "$root/XeMTLDriver.bundle" "$repair/XeMTLDriver.bundle"
ditto --norsrc "$runtime" "$repair/XeMTLDriver.bundle/Contents/MacOS/air-runtime"
codesign -f -s - "$repair/XeMTLDriver.bundle" > "$repair/signature.log" 2>&1
codesign --verify --strict "$repair/XeMTLDriver.bundle"
mkdir -p /Library/GPUBundles
for name in XeMTLDriver.bundle xemtl; do
    if [ -e "/Library/GPUBundles/$name" ]; then ditto --norsrc "/Library/GPUBundles/$name" "$repair/live-before-$name"; fi
done
for name in XeMTLDriver.bundle xemtl; do
    from="$root/$name"
    [ "$name" != XeMTLDriver.bundle ] || from="$repair/$name"
    ditto --norsrc "$from" "/Library/GPUBundles/$name.repaired-new"
    rm -rf "/Library/GPUBundles/$name"
    mv "/Library/GPUBundles/$name.repaired-new" "/Library/GPUBundles/$name"
    chown -R root:wheel "/Library/GPUBundles/$name"
    chmod -R go-w "/Library/GPUBundles/$name"
done
printf '# Runtime repair: probe first\nmtlprobe\n' > /Library/GPUBundles/xemtl-allow.txt
touch /Library/GPUBundles/.xemtl-fbfetch-ws
chown root:wheel /Library/GPUBundles/xemtl-allow.txt /Library/GPUBundles/.xemtl-fbfetch-ws
chmod 644 /Library/GPUBundles/xemtl-allow.txt /Library/GPUBundles/.xemtl-fbfetch-ws
shasum -a 256 /Library/GPUBundles/XeMTLDriver.bundle/Contents/MacOS/air-runtime/* > "$repair/air-runtime.sha256"
echo AIR_RUNTIME_INSTALLED_RUNNING_METAL_PROBE
NM_TEST_PLUGIN=/Library/GPUBundles/XeMTLDriver.bundle/Contents/MacOS/XeMTLDriver XEMTL_LOG_PATH="$repair/mtlprobe-plugin.log" "$root/mtlprobe" > "$repair/mtlprobe-result.txt" 2>&1 || {
    tail -35 "$repair/mtlprobe-result.txt"
    tail -35 "$repair/mtlprobe-plugin.log"
    echo METAL_PROBE_STILL_FAILED_NO_COMPOSITOR_RESTART > "$repair/RESULT"
    exit 1
}
grep -q 'metal compute: OK' "$repair/mtlprobe-result.txt"
grep -q 'metal draw: OK' "$repair/mtlprobe-result.txt"
cat "$repair/mtlprobe-result.txt"
# Preserve the complete runtime for any later startup of this same checked package.
ditto --norsrc "$root/XeMTLDriver.bundle" "$repair/payload-bundle-before"
rm -rf "$root/XeMTLDriver.bundle"
ditto --norsrc "$repair/XeMTLDriver.bundle" "$root/XeMTLDriver.bundle"
chown -R root:wheel "$root/XeMTLDriver.bundle"
chmod -R go-w "$root/XeMTLDriver.bundle"
printf '# Native DP-H desktop, complete AIR runtime\n!WindowServer\nmtlprobe\nDisplayApply\n' > /Library/GPUBundles/xemtl-allow.txt
pgrep -x WindowServer > "$repair/windowserver-before.txt" || true
echo METAL_COMPUTE_AND_DRAW_PASSED_RESTARTING_GRAPHICS_SESSION_ONLY
launchctl kickstart -k system/com.apple.WindowServer > "$repair/windowserver-kickstart.txt" 2>&1
touch "$repair/COMPOSITOR_RESTARTED"
printf 'Metal 计算与绘制已通过。已重新启动图形会话，请在黑苹果正常登录。\n'
for n in $(seq 1 300); do
    if [ "$(stat -f '%Su' /dev/console)" = hackintosh ]; then break; fi
    sleep 2
done
if [ "$(stat -f '%Su' /dev/console)" = hackintosh ]; then
    launchctl asuser 501 sudo -u hackintosh /usr/bin/open -W -n "$root/DisplayApply.app" --args --select-hidpi > "$repair/hidpi-apply.log" 2>&1 || true
    cp /Users/hackintosh/Library/Caches/WTLArrowNative/display-audit.json "$repair/display-audit.json" || true
fi
sleep 10
ws=$(pgrep -x WindowServer | head -1)
printf '%s\n' "$ws" > "$repair/windowserver-after.txt"
sample "$ws" 1 20 -file "$repair/windowserver.sample.txt" >/dev/null 2>&1 || true
log show --last 5m --style compact --predicate "processID == $ws AND eventMessage CONTAINS[c] 'framebuffer fetch'" > "$repair/windowserver-fetch.log" 2>&1 || true
echo COMPLETE_RUNTIME_PROBE_PASSED_COMPOSITOR_RESTARTED_PENDING_DISPLAY_ACCEPTANCE > "$repair/RESULT"
