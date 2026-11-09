#!/bin/bash
set -u
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
root='/Library/Application Support/WTLArrowTiming.EXAMPLE'
restore="$root/WiFiD3Restore.1"
[ "$(sysctl -n kern.bootsessionuuid)" != "$(cat "$restore/prepared-boot")" ] || exit 0
job=/Library/LaunchDaemons/org.local.wtl-wifi-d3-restore-once.plist
[ ! -f "$job" ] || mv "$job" "$restore/collector-consumed.plist"
out='/Users/hackintosh/Library/Logs/WiFiD3Restore.1'
mkdir -p "$out"
chmod 750 "$out"
chown hackintosh:staff "$out"
exec >> "$out/boot.log" 2>&1
tries=0
for step in $(seq 1 72); do
 snapshot="$out/scalar-latest.txt"
 {
  echo "BA_SAMPLE=$step"
  echo RESTORE_D3=1
  printf 'BOOT_UUID='; sysctl -n kern.bootsessionuuid
  printf 'CONSOLE='; stat -f '%Su' /dev/console
  kmutil showloaded --list-only -b com.senmiko.AirportRTW 2>/dev/null || true
  ioreg -r -c AirportRTW -l -d 1 -w0 | grep -E '"(RTWLocalPowerStage|RTWLocalPowerResult|RTW89NegotiatedPHY|RTWStartupFailed|IOBSDName)"' || true
  printf 'WIFI_IP='; ipconfig getifaddr en2 2>/dev/null || true
  sysctl debug.xegfx_stage debug.xegfxaccel_boot debug.xegfxaccel_iop_xe_flips debug.xegfx_agdc_k5_why 2>/dev/null || true
  if [ "$(stat -f '%Su' /dev/console)" = hackintosh ]; then
   if bash "$root/health.sh"; then
    if [ ! -e "$out/HTTPS_VERIFIED" ] && [ "$tries" -lt 3 ] && ipconfig getifaddr en2 >/dev/null 2>&1; then
     tries=$((tries+1))
     code=$(curl --interface en2 -fsS --connect-timeout 3 --max-time 8 -o "$out/wifi-success.html" -w '%{http_code}' https://www.apple.com/library/test/success.html 2>/dev/null || true)
     if [ "$code" = 200 ]; then touch "$out/HTTPS_VERIFIED"; fi
    fi
   fi
  fi
  [ ! -e "$out/HTTPS_VERIFIED" ] || echo BOUND_WIFI_HTTPS_200_VERIFIED
 } > "$snapshot" 2>&1
 cat "$snapshot"
 curl -fsS --connect-timeout 2 --max-time 4 --data-binary "@$snapshot" '__LOCAL_RECEIVER_URL__' >/dev/null 2>&1 || true
 sleep 5
done
chown -R hackintosh:staff "$out"
echo D3_RESTORE_COLLECTION_COMPLETE
