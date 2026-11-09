#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
root='/Library/Application Support/WTLArrowTiming.EXAMPLE'
trial="$root/NativeBATrial.1"
[ "$(sysctl -n kern.bootsessionuuid)" != "$(cat "$trial/prepared-boot")" ] || exit 0
job=/Library/LaunchDaemons/org.local.wtl-native-ba1-once.plist
[ ! -f "$job" ] || mv "$job" "$trial/collector-consumed.plist"
out='/Users/hackintosh/Library/Logs/NativeBATrial.1'
mkdir -p "$out"
chmod 750 "$out"
chown hackintosh:staff "$out"
exec >> "$out/boot.log" 2>&1
for step in $(seq 1 60); do
 snapshot="$out/scalar-latest.txt"
 {
  echo "BA_SAMPLE=$step"
  printf 'BOOT_UUID='; sysctl -n kern.bootsessionuuid
  printf 'CONSOLE='; stat -f '%Su' /dev/console
  kmutil showloaded --list-only -b com.senmiko.AirportRTW 2>/dev/null || true
  ioreg -r -c AirportRTW -l -d 1 -w0 | grep -E '"(RTWBA[A-Za-z0-9]+|RTWNativeRx[A-Za-z0-9]+|RTW89(RxBAAgreements|AmpduRxStartCount|AmpduRxStartFailCount|TxAmpduMask|TxDescAggregationEnabled|RxCCMPReplays|RxCCMPIntegrityFailures))" = (Yes|No|[0-9]+|"[^"]*")$' || true
  sysctl debug.xegfx_stage debug.xegfxaccel_boot debug.xegfxaccel_iop_xe_flips debug.xegfx_agdc_k5_why 2>/dev/null || true
  for process in ControlCenter SystemUIServer; do pgrep -x "$process" || true; done
  if [ "$(stat -f '%Su' /dev/console)" = hackintosh ]; then bash "$root/health.sh" || true; fi
 } > "$snapshot" 2>&1
 cat "$snapshot"
 curl -fsS --connect-timeout 2 --max-time 4 --data-binary "@$snapshot" '__LOCAL_RECEIVER_URL__' >/dev/null 2>&1 || true
 sleep 5
done
echo BA_COLLECTION_COMPLETE
chown -R hackintosh:staff "$out"
