#!/bin/bash
set -uo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
root='/Library/Application Support/WTLArrowTiming.EXAMPLE'
trial="$root/BluetoothTrial.5"
boot=$(sysctl -n kern.bootsessionuuid)
[ "$boot" != DF4A3FDB-2FDE-542A-A032-9043C421C78D ] || exit 0
mkdir "$trial/collection-started" 2>/dev/null || exit 0
mkdir -p "$trial/postboot"
for n in $(jot 60 1); do
 file="$trial/postboot/sample-$n.txt"
 {
  echo BT_SAMPLE="$n"
  echo BOOT_UUID="$boot"
  echo BT_SCAN_TRIAL=1
  date -u
  kmutil showloaded --list-only 2>/dev/null | grep -E 'RealtekBluetoothFirmware|BlueToolFixup|AirportRTW|WTLArrowEarlyClaim|XeGfxDisplay'
  ioreg -r -c BlueToolFixup -l -w0 | grep -E 'WTLRtl8922Trial|VersionInfo' || true
  ioreg -r -c RealtekBluetoothFirmware -l -w0 | grep -E 'fw_name|FirmwareLoaded' || true
  ioreg -p IOUSB -l -w0 | grep '"FirmwareLoaded"' || true
  /usr/bin/perl -e 'alarm 15; exec @ARGV' system_profiler SPBluetoothDataType -detailLevel mini | grep -E '^[[:space:]]+(State|Chipset|Firmware Version|Transport|Vendor ID):'
  log show --last 25s --style compact --info --predicate 'process == "bluetoothd" AND (eventMessage CONTAINS "FoundBluetoothInterfaces" OR eventMessage CONTAINS "Found external Bluetooth dongle" OR eventMessage CONTAINS "HCI initialization" OR eventMessage CONTAINS "SetEventMaskPage2" OR eventMessage CONTAINS "Stack initialization complete" OR eventMessage CONTAINS "Reset scan in LE_Scan" OR eventMessage CONTAINS "Scan stopped improperly" OR eventMessage CONTAINS "Bluetooth error - restarting")' 2>/dev/null | tail -18
  if [ "$((n%6))" = 0 ]; then bash "$root/health.sh" 2>&1; fi
  echo END_BT_SAMPLE
 } > "$file" 2>&1
 curl --noproxy '*' -fsS --connect-timeout 3 --max-time 5 -X POST --data-binary "@$file" '__ENDPOINT__' >/dev/null 2>&1 || true
 sleep 10
done
