#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
trap 'echo "RUNTIME_TEST_FAILED_LINE=$LINENO"' ERR
[ "$(id -u)" = 0 ]
[ "$(sysctl -n kern.bootsessionuuid)" = 48294D9D-66CD-5962-BD0A-F9D88E3989CA ]
root='/Library/Application Support/WTLArrowTiming.EXAMPLE'
if ! bash "$root/health.sh"; then bash -x "$root/health.sh"; fi
work="$root/BluetoothRuntimeSwitch.2"
[ ! -e "$work" ]
mkdir -m 700 "$work"
for key in bluetoothHostControllerSwitchBehavior bluetoothExternalDongleFailed bluetoothInternalControllerInfo; do
    if ! nvram -x "$key" > "$work/$key.before.plist" 2>/dev/null; then rm -f "$work/$key.before.plist"; fi
done
restore_flags() {
    for key in bluetoothHostControllerSwitchBehavior bluetoothExternalDongleFailed bluetoothInternalControllerInfo; do
        if [ -f "$work/$key.before.plist" ]; then nvram -x -f "$work/$key.before.plist"; else nvram -d "$key" >/dev/null 2>&1 || true; fi
    done
}
changed=0
accepted=0
finish() {
    rc=$?
    trap - EXIT
    set +e
    if [ "$changed" = 1 ] && [ "$accepted" = 0 ]; then
        restore_flags
        killall -TERM bluetoothd 2>/dev/null || true
        echo TEMPORARY_BT_SELECTION_FLAGS_RESTORED_NO_RESTART
    fi
    exit "$rc"
}
trap finish EXIT
# Change only controller-selection policy; no firmware/kext/EFI/cache writes.
changed=1
nvram bluetoothHostControllerSwitchBehavior=always
nvram bluetoothExternalDongleFailed=%00
nvram bluetoothInternalControllerInfo=%00%00%00%00%00%00%00%00%00%00%00%00%00
nvram bluetoothHostControllerSwitchBehavior
nvram bluetoothExternalDongleFailed
nvram bluetoothInternalControllerInfo
killall -TERM bluetoothd 2>/dev/null || true
for n in $(jot 8 1); do
    sleep 5
    system_profiler SPBluetoothDataType -detailLevel mini > "$work/report-$n.txt"
    grep -E '^[[:space:]]+(State|Chipset|Firmware Version|Transport):' "$work/report-$n.txt" || true
    if grep -q 'Chipset: THIRD_PARTY_DONGLE' "$work/report-$n.txt"; then
        accepted=1
        echo REALTEK_EXTERNAL_CONTROLLER_PATH_SELECTED_NOT_YET_PAIRED
        break
    fi
done
/usr/bin/log show --last 2m --style compact --info --predicate 'process == "bluetoothd" AND (eventMessage CONTAINS "FoundBluetooth" OR eventMessage CONTAINS "chipset to" OR eventMessage CONTAINS "Use internal" OR eventMessage CONTAINS "STATUS" OR eventMessage CONTAINS "Initialization failed" OR eventMessage CONTAINS "HCI initialization" OR eventMessage CONTAINS "Transport layer" OR eventMessage CONTAINS "GetExternalDongle")' > "$work/transport-result.log" 2>&1 || true
tail -70 "$work/transport-result.log"
bash "$root/health.sh" || true
if [ "$accepted" = 1 ]; then
    echo TEMPORARY_BT_SELECTION_FLAGS_RETAINED_FOR_VALIDATION_NO_RESTART
else
    echo NO_USABLE_EXTERNAL_CONTROLLER_SELECTED_RESTORING_PREVIOUS_FLAGS
fi

echo CURRENT_USB_BT_TEST_COMPLETE_NO_MACHINE_RESTART
