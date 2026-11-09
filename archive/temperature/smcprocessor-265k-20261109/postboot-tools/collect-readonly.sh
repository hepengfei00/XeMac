export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
graphics_root='/Library/Application Support/WTLArrowTiming.EXAMPLE'
sensor_pkg="$graphics_root/CPUTemperature.1"
boot=$(sysctl -n kern.bootsessionuuid)
printf '\n=== CURRENT BOOT ===\n'
date
sw_vers
printf 'BOOT_UUID=%s\n' "$boot"
uptime
sysctl -n kern.bootargs
printf '\n=== CURRENT SENSOR MODULE ===\n'
kmutil showloaded --list-only 2>&1 | grep -Ei 'SMCProcessor|VirtualSMC|Lilu|AirportRTW|BlueToolFixup|RealtekBluetoothFirmware'
ioreg -r -c SMCProcessor -l -w0
printf '\n=== AUTOMATIC NEXT-BOOT COLLECTION ===\n'
for name in collected-boot latest-result-path.txt; do
    [ ! -f "$sensor_pkg/$name" ] || cat "$sensor_pkg/$name"
done
out="$sensor_pkg/results/$boot"
if [ -d "$out" ]; then
    for name in RESULT collector.log loaded-sensor.txt sensor-service.txt temperature-keys.log powermetrics-smc.txt sensor-events.log; do
        printf '\nFILE=%s\n' "$name"
        [ ! -f "$out/$name" ] || tail -n 220 "$out/$name"
    done
else
    echo NO_COLLECTION_FOR_CURRENT_BOOT
    tail -n 60 "$sensor_pkg/launchd.log" 2>/dev/null
fi
printf '\n=== CURRENT DAILY PROFILE ===\n'
shasum -a 256 "$graphics_root/trial-config.plist"
plutil -extract Misc.Boot.Timeout raw -o - "$graphics_root/trial-config.plist"
printf '\n=== CURRENT GRAPHICS HEALTH ===\n'
bash "$graphics_root/health.sh"
printf '\n=== IOVERSATILE BUNDLE METADATA ===\n'
find /System/Library/Extensions /Library/Extensions -maxdepth 4 -iname '*versatile*' -print 2>/dev/null
for path in /System/Library/Extensions/IOVersatile.kext/Contents/Info.plist /Library/Extensions/IOVersatile.kext/Contents/Info.plist; do
    if [ -f "$path" ]; then
        printf '\nBUNDLE=%s\n' "$path"
        plutil -p "$path"
    fi
done
printf '\n=== LOADED VERSATILE DEPENDENCIES ===\n'
kmutil showloaded --list-only 2>&1 | grep -Ei 'IOVersatile|IOAccelerator|IOGraphics|IOGPUFamily|IOSurface'
printf '\n=== CURRENT BOOT ERRORS ONLY ===\n'
epoch=$(sysctl -n kern.boottime | awk '{print $4}' | tr -d ',')
start=$(date -r "$epoch" '+%Y-%m-%d %H:%M:%S')
log show --start "$start" --style compact --predicate '(process == "kernel") AND ((eventMessage CONTAINS "IOVersatile") OR (eventMessage CONTAINS "SMCProcessor"))' 2>&1 | tail -n 180
printf '\nPOSTBOOT_TEMPERATURE_AND_BOOT_LOG_READ_ONLY_FINISHED\n'
