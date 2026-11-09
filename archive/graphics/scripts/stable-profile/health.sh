#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
root=$(cd "$(dirname "$0")" && pwd)
[ "$(sysctl -n debug.xegfx_stage)" = 7 ]
[ "$(sysctl -n debug.xegfxaccel_boot)" = 2 ]
[ "$(stat -f '%Su' /dev/console)" = hackintosh ]
[ "$(sysctl -n debug.xegfx_agdc_k5_caps)" -gt 0 ]
[ "$(sysctl -n debug.xegfx_agdc_k5_link)" -gt 0 ]
[ "$(sysctl -n debug.xegfx_agdc_k5_why)" = 0 ]
[ "$(sysctl -n debug.xegfxaccel_iop_xe_flips)" -gt 0 ]
kmutil showloaded --list-only -b org.watchthelight.XeGfxDisplay | grep -Fq B7EC9618-866F-3E7B-8C28-7E3686A981EC
[ "$(shasum -a 256 /Library/GPUBundles/XeMTLDriver.bundle/Contents/MacOS/XeMTLDriver | awk '{print $1}')" = 6f2e9c5b6306ee132ccca967e911a42d904c85f5f94917f6b65066fd007de696 ]
for pair in 'pixel_width 3840' 'pixel_height 2160' 'logical_width 1920' 'logical_height 1080' 'refresh_hz 60' 'hidpi 1'; do
 read -r key wanted <<< "$pair"
 [ "$(plutil -extract "0.current.$key" raw -o - "$root/display-audit.json")" = "$wanted" ]
done
boot_epoch=$(sysctl -n kern.boottime | awk '{print $4}' | tr -d ',')
[ "$(stat -f '%m' "$root/display-audit.json")" -ge "$boot_epoch" ]
echo NATIVE_4K60_HIDPI_METAL_SCANOUT_HEALTH_CONFIRMED
