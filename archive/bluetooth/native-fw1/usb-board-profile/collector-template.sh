#!/bin/bash
set -uo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
root='/Library/Application Support/WTLArrowTiming.EXAMPLE'
trial="$root/BluetoothTrial.3"
boot=$(sysctl -n kern.bootsessionuuid)
[ "$boot" != CDFC4E42-A2F2-5A4C-8B49-7B350C77E009 ] || exit 0
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
    /usr/bin/log show --last 1m --style compact --info --predicate 'process == "bluetoothd" AND (eventMessage CONTAINS "FindTransportType" OR eventMessage CONTAINS "chipset to" OR eventMessage CONTAINS "FoundBluetooth" OR eventMessage CONTAINS "Use internal" OR eventMessage CONTAINS "STATUS" OR eventMessage CONTAINS "Initialization failed" OR eventMessage CONTAINS "Transport layer" OR eventMessage CONTAINS "HCI initialization")' 2>/dev/null | tail -20
    log show --last 1m --style compact --predicate 'eventMessage CONTAINS "RealtekFirmware"' 2>/dev/null | tail -20
    if [ "$((n%6))" = 0 ]; then bash "$root/health.sh" 2>&1; fi
    echo END_BT_SAMPLE
  } > "$file" 2>&1
  curl --noproxy '*' -fsS --connect-timeout 3 --max-time 5 -X POST --data-binary "@$file" '__ENDPOINT__' > /dev/null 2>&1 || true
  sleep 10
done
