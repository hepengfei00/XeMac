#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
trap 'echo "BOUNDED_TEST_ERROR_LINE=$LINENO"' ERR
[ "$(id -u)" = 0 ]
[ "$(sysctl -n kern.bootsessionuuid)" = 48294D9D-66CD-5962-BD0A-F9D88E3989CA ]
root='/Library/Application Support/WTLArrowTiming.EXAMPLE'
bash "$root/health.sh"
work="$root/BluetoothBoundedTrial.1"
[ ! -e "$work" ]
mkdir -m 700 "$work"
keys=(bluetoothHostControllerSwitchBehavior bluetoothExternalDongleFailed bluetoothInternalControllerInfo)
for key in "${keys[@]}"; do
 if ! nvram -x "$key" > "$work/$key.before.plist" 2>/dev/null; then rm -f "$work/$key.before.plist"; fi
done
changed=0
accepted=0
finish() {
 rc=$?
 trap - EXIT
 set +e
 if [ "$changed" = 1 ] && [ "$accepted" = 0 ]; then
  for key in "${keys[@]}"; do
   if [ -f "$work/$key.before.plist" ]; then nvram -x -f "$work/$key.before.plist"; else nvram -d "$key" >/dev/null 2>&1 || true; fi
  done
  killall -TERM bluetoothd 2>/dev/null || true
  echo BT_BOUND_TEST_FLAGS_RESTORED_NO_MACHINE_RESTART
 fi
 exit "$rc"
}
trap finish EXIT
start=$(date '+%Y-%m-%d %H:%M:%S')
changed=1
nvram bluetoothHostControllerSwitchBehavior=always
nvram bluetoothExternalDongleFailed=%00
# Target 25G332 bluetoothd requires CFDataGetLength == 14 before calling external discovery.
# Previous 13-byte test failed this guard and therefore did not test discovery.
info=''
for n in $(jot 14 1); do info="${info}%00"; done
nvram "bluetoothInternalControllerInfo=$info"
"$(dirname "$0")/check-bt-gate"
killall -TERM bluetoothd 2>/dev/null || true
for n in $(jot 6 1); do
 sleep 4
 system_profiler SPBluetoothDataType -detailLevel mini > "$work/report-$n.txt"
 grep -E '^[[:space:]]+(State|Chipset|Firmware Version|Transport):' "$work/report-$n.txt" || true
 if grep -q 'State: On' "$work/report-$n.txt" && ! grep -q 'Chipset: BCM_4350C2' "$work/report-$n.txt"; then
  accepted=1
  echo BT_CONTROLLER_ON_REQUIRES_USER_SCAN_PAIR_VALIDATION
  break
 fi
done
/usr/bin/log show --start "$start" --style compact --info --debug --predicate 'process == "bluetoothd" AND (eventMessage CONTAINS "HasInternalControllerAddr" OR eventMessage CONTAINS "Found_USB_Dongle" OR eventMessage CONTAINS "FoundBluetoothInterfaces" OR eventMessage CONTAINS "External Bluetooth dongle" OR eventMessage CONTAINS "Found external Bluetooth" OR eventMessage CONTAINS "SwitchBehavior" OR eventMessage CONTAINS "Initialization failed" OR eventMessage CONTAINS "Transport layer" OR eventMessage CONTAINS "HCI initialization" OR eventMessage CONTAINS "SetEventMaskPage2")' > "$work/discovery.log" 2>&1 || true
cat "$work/discovery.log"
bash "$root/health.sh"
if [ "$accepted" = 1 ]; then
 echo BT_SELECTION_FLAGS_RETAINED_PENDING_PAIRING_CHECK
else
 echo BT_NOT_ON_RESTORING_FLAGS
fi
echo BT_14_BYTE_GATE_TEST_COMPLETE_NO_MACHINE_RESTART
