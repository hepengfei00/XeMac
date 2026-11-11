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
bash "$root/probe-display.sh" audit
echo NATIVE_4K60_HIDPI_METAL_SCANOUT_HEALTH_CONFIRMED
