#!/bin/bash
set -uo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
root='/Library/Application Support/WTLArrowTiming.EXAMPLE'
trial="$root/BluetoothTrial.1"
boot=$(sysctl -n kern.bootsessionuuid)
[ "$boot" != B9298AB5-6A9D-5222-B971-FD93F087ECEB ] || exit 0
mkdir "$trial/collection-started" 2>/dev/null || exit 0
mkdir -p "$trial/postboot"
for n in $(jot 90 1); do
  file="$trial/postboot/sample-$n.txt"
  {
    echo BT_SAMPLE="$n"
    echo BOOT_UUID="$boot"
    date -u
    kmutil showloaded --list-only 2>/dev/null | grep -E 'RealtekBluetoothFirmware|BlueToolFixup|AirportRTW|WTLArrowEarlyClaim|XeGfxDisplay'
    ioreg -r -c RealtekBluetoothFirmware -l -w0 | grep -E 'fw_name|FirmwareLoaded' || true
    ioreg -p IOUSB -l -w0 | grep -E '"FirmwareLoaded"' || true
    system_profiler SPBluetoothDataType -detailLevel mini | grep -E '^[[:space:]]+(State|Chipset|Firmware Version|Transport|Vendor ID):'
    log show --last 1m --style compact --predicate 'eventMessage CONTAINS "RealtekFirmware"' 2>/dev/null | tail -20
    if [ "$((n%6))" = 0 ]; then bash "$root/health.sh" 2>&1; fi
    echo END_BT_SAMPLE
  } > "$file" 2>&1
  curl --noproxy '*' -fsS --connect-timeout 3 --max-time 5 -X POST --data-binary "@$file" '__ENDPOINT__' > /dev/null 2>&1 || true
  sleep 10
done
