#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
[ "$(id -u)" = 0 ]
[ "$(sysctl -n kern.bootsessionuuid)" = E855C422-C1CF-54EA-8518-F8B2D44A4D7B ]
stage=$(cd "$(dirname "$0")" && pwd)
root='/Library/Application Support/WTLArrowTiming.EXAMPLE'
live="$root/NativeBALive.1"
cd "$stage"
shasum -a 256 --strict --quiet -c SHA256SUMS
shasum -a 256 --strict --quiet -c graphics-baseline.sha256
codesign --verify --deep --strict AirportRTW.kext
[ "$(shasum -a 256 AirportRTW.kext/Contents/MacOS/AirportRTW | awk '{print $1}')" = acdd870da84b7dd4892b101d7f1c67fdc730e68ca0ca6ebdef86c1e413d8df27 ]
bash "$root/health.sh"
[ -z "$(kmutil showloaded --list-only 2>/dev/null | grep -E 'com.senmiko.AirportRTW|org.local.WTLRTW89BackendDiag' || true)" ]
cmp "$root/Arrow-Native-candidate.kc" /Library/KernelCollections/AuxiliaryKernelExtensions.kc
[ ! -e "$live" ]
mkdir -m 700 "$live"
ditto --norsrc AirportRTW.kext "$live/AirportRTW.kext"
chown -R root:wheel "$live"
cp /Library/KernelCollections/AuxiliaryKernelExtensions.kc "$live/Aux-before.kc"
. "$root/esp.sh"
restore_cache() {
 if ! cmp -s "$live/Aux-before.kc" /Library/KernelCollections/AuxiliaryKernelExtensions.kc; then write_collection "$live/Aux-before.kc"; fi
 cmp "$live/Aux-before.kc" /Library/KernelCollections/AuxiliaryKernelExtensions.kc
}
finish() { rc=$?; trap - EXIT; restore_cache; echo "BA_CURRENT_SESSION_CHECK_EXIT=$rc NO_RESTART_PERFORMED"; exit "$rc"; }
trap finish EXIT
capture() {
 kmutil showloaded --list-only -b com.senmiko.AirportRTW 2>/dev/null || true
 ioreg -r -c AirportRTW -l -d 1 -w0 | grep -E '"(RTWBA[A-Za-z0-9]+|RTWNativeRx[A-Za-z0-9]+|RTWLocalPowerStage|IOBSDName)"' || true
}
run_load() {
 local style=$1 child running=1
 kmutil load --no-authorization --load-style "$style" -p "$live/AirportRTW.kext" > "$live/$style.log" 2>&1 &
 child=$!
 for step in $(seq 1 15); do
  if ! kill -0 "$child" 2>/dev/null; then running=0; break; fi
  sleep 2
 done
 if [ "$running" = 1 ]; then
  kill -TERM "$child" 2>/dev/null || true
  echo BA_LOAD_STILL_PENDING_STOPPING_WITHOUT_RETRY
  cat "$live/$style.log"
  return 1
 fi
 wait "$child" || true
 cat "$live/$style.log"
}
run_load start-only
kmutil showloaded --list-only -b com.senmiko.AirportRTW > "$live/loaded-after-start.txt" 2>/dev/null || true
if grep -Fq 33E15E45-2C19-3169-8D57-FF3D601133B3 "$live/loaded-after-start.txt"; then
 echo D5_CORRECT_UUID_LOADED_STARTING_HARDWARE_MATCH
 run_load match-only
 for step in $(seq 1 12); do capture >> "$live/runtime.txt" 2>&1; sleep 3; done
 cat "$live/runtime.txt"
 echo D5_LIVE_RESULT_CAPTURED_NO_RESTART
else
 echo D5_NOT_LOADED_RESTORING_CACHE_BEFORE_ONE_BOOT_PREPARATION
 restore_cache
 bash "$stage/install.sh"
 echo D5_ONE_BOOT_PACKAGE_PREPARED_AWAITING_NEW_RESTART_AUTHORIZATION
fi
bash "$root/health.sh"
restore_cache
echo D5_SPEED_DIAG_PREPARATION_COMPLETE_NO_RESTART
