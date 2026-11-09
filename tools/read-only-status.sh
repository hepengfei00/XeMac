#!/bin/bash
# SPDX-License-Identifier: MIT
# Read-only local overview. Output may include user-specific names.
set -eu
export PATH=/usr/bin:/bin:/usr/sbin:/sbin

if [ "$(uname -s)" != Darwin ]; then
    echo 'This status reader requires macOS.' >&2
    exit 1
fi

sw_vers
uname -m
sysctl -n machdep.cpu.brand_string
sysctl hw.memsize kern.bootsessionuuid kern.boottime
system_profiler SPDisplaysDataType
kmutil showloaded --list-only 2>/dev/null | \
    grep -E 'XeGfx|EarlyClaim|AirportRTW|RealtekBluetooth|BlueToolFixup|SMCProcessor' || true
networksetup -listallhardwareports
for key in debug.xegfx_stage debug.xegfxaccel_boot debug.xegfxaccel_iop_xe_flips; do
    sysctl "$key" 2>/dev/null || true
done
