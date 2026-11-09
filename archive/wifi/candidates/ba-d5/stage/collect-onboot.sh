#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
root='/Library/Application Support/WTLArrowTiming.EXAMPLE/NativeBATrial.1'
[ "$(sysctl -n kern.bootsessionuuid)" != "$(cat "$root/prepared-boot")" ] || exit 0
job=/Library/LaunchDaemons/org.local.wtl-native-ba1-once.plist
[ ! -f "$job" ] || mv "$job" "$root/collector-consumed.plist"
out='/Users/hackintosh/Library/Logs/NativeBATrial.1'
mkdir -p "$out"
chmod 750 "$out"
chown hackintosh:staff "$out"
exec >> "$out/boot.log" 2>&1
date -u
sysctl -n kern.bootsessionuuid
for step in $(seq 1 48); do
 echo "SAMPLE=$step"
 kmutil showloaded --list-only -b com.senmiko.AirportRTW 2>/dev/null || true
 for cls in IOPCIEDeviceWrapper IOPCIDevice AirportRTW AirportRTWSkywalkInterface; do
  ioreg -r -c "$cls" -l -d 1 -w0 | grep -E 'class |"RTW|"IOPowerManagement"|"IOBSDName"' || true
 done
 ps -axo pid,state,comm | grep -E 'ControlCenter.app|SystemUIServer.app' || true
 sysctl debug.xegfx_stage debug.xegfxaccel_boot 2>/dev/null || true
 sleep 5
done
log show --last 5m --style compact --predicate 'process == "kernel" AND (eventMessage CONTAINS "RTWLocal231" OR eventMessage CONTAINS "AirportRTW: startup" OR eventMessage CONTAINS "rtw89" OR eventMessage CONTAINS "SSM_EVENT" OR eventMessage CONTAINS "SCAN_MANAGER")' > "$out/kernel.log" 2>&1 || true
echo WIFI_DIAGNOSTIC_COLLECTION_COMPLETE
chown -R hackintosh:staff "$out"
