#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
umask 022
[ "$(id -u)" = 0 ]
pkg=$(cd "$(dirname "$0")" && pwd)
boot=$(sysctl -n kern.bootsessionuuid)
[ ! -f "$pkg/COLLECTION_DISABLED" ] || exit 0
if [ "$boot" = "$(cat "$pkg/prepared-boot")" ]; then
    echo WAITING_FOR_NEXT_BOOT_NO_ACTION
    exit 0
fi
[ ! -f "$pkg/collected-boot" ] || exit 0
mkdir -p "$pkg/results" "$pkg/collector-locks"
mkdir "$pkg/collector-locks/$boot" 2>/dev/null || exit 0
out="$pkg/results/$boot"
mkdir "$out"
exec >> "$out/collector.log" 2>&1
printf 'BOOT=%s\n' "$boot"
date
sw_vers
sysctl hw.physicalcpu hw.logicalcpu hw.packages
ready=0
for ((i=0; i<60; i++)); do
    kmutil showloaded --list-only -b as.vit9696.SMCProcessor > "$out/loaded-sensor.txt" 2>&1 || true
    if grep -Fq BE9CB674-03BD-3CAC-8CD5-E3E60941A67E "$out/loaded-sensor.txt"; then
        ready=1
        break
    fi
    sleep 2
done
ioreg -r -c SMCProcessor -l -w0 > "$out/sensor-service.txt"
kmutil showloaded --list-only > "$out/loaded-kexts.txt" 2>&1 || true
if [ "$ready" = 1 ]; then
    sleep 2
    for ((i=1; i<=6; i++)); do
        printf '\nSAMPLE=%s TIME=' "$i" >> "$out/temperature-keys.log"
        date '+%Y-%m-%dT%H:%M:%S%z' >> "$out/temperature-keys.log"
        # The official reader's -s mode reads SMC keys. Keep only CPU temperature
        # keys and reader failures; do not retain machine identity or other keys.
        perl -e 'alarm shift; exec @ARGV or die $!' 15 "$pkg/smcread" -s 2>&1 |
            awk '/^\[TC/ || /Unable|NOT READABLE|[Ee]rror|[Ff]ailed/' >> "$out/temperature-keys.log" || true
        sleep 3
    done
    perl -e 'alarm shift; exec @ARGV or die $!' 15 /usr/bin/powermetrics --samplers smc -n 1 -i 1000 > "$out/powermetrics-smc.txt" 2>&1 || true
    echo MODULE_LOADED_SAMPLES_RECORDED_PENDING_READING_VALIDATION > "$out/RESULT"
else
    echo EXPECTED_SENSOR_NOT_LOADED_NO_TEMPERATURE_SUCCESS_CLAIM > "$out/RESULT"
fi
log show --last 5m --style compact --predicate 'eventMessage CONTAINS "SMCProcessor" OR eventMessage CONTAINS "failed to get cpu topology" OR eventMessage CONTAINS "failed to find a compatible processor"' > "$out/sensor-events.log" 2>&1 || true
printf '%s\n' "$boot" > "$pkg/collected-boot"
printf '%s\n' "$out" > "$pkg/latest-result-path.txt"
echo NEXT_BOOT_CPU_TEMPERATURE_COLLECTION_FINISHED_NO_RESTART
