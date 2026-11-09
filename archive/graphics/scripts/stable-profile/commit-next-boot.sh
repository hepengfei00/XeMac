#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
[ "$(id -u)" = 0 ]
root=$(cd "$(dirname "$0")" && pwd)
bash "$root/health.sh"
[ "$(shasum -a 256 "$root/Arrow-Native-candidate.kc" | awk '{print $1}')" = 910da863fd31d575d792fdd3f2bb2bf59c295010fe2cc2a584e6ceffaf52fa27 ]
. "$root/esp.sh"
committed=0
cleanup() {
 rc=$?
 trap - EXIT
 set +e
 unmount_esp
 if [ "$committed" = 0 ]; then bash "$root/restore.sh"; fi
 exit "$rc"
}
trap cleanup EXIT
mount_esp
if ! cmp -s "$root/stable-config.plist" "$esp_config"; then cmp -s "$root/trial-config.plist" "$esp_config"; fi
if [ -e "$esp_mp/EFI/OC/Kexts/WTLArrowTrial3" ]; then
 cmp -s "$root/WTLArrowEarlyClaim.kext/Contents/MacOS/WTLArrowEarlyClaim" "$esp_mp/EFI/OC/Kexts/WTLArrowTrial3/WTLArrowEarlyClaim.kext/Contents/MacOS/WTLArrowEarlyClaim"
 cmp -s "$root/WTLArrowEarlyClaim.kext/Contents/Info.plist" "$esp_mp/EFI/OC/Kexts/WTLArrowTrial3/WTLArrowEarlyClaim.kext/Contents/Info.plist"
else
 mkdir "$esp_mp/EFI/OC/Kexts/WTLArrowTrial3"
 ditto --norsrc "$root/WTLArrowEarlyClaim.kext" "$esp_mp/EFI/OC/Kexts/WTLArrowTrial3/WTLArrowEarlyClaim.kext"
fi
write_collection "$root/Arrow-Native-candidate.kc"
nvram -x -f "$root/gates.plist"
"$root/gate-audit" > "$root/stable-armed-gates.txt"
write_config "$root/trial-config.plist"
cmp -s "$root/trial-config.plist" "$esp_config"
shasum -a 256 "$esp_config" /Library/KernelCollections/AuxiliaryKernelExtensions.kc > "$root/stable-enabled.sha256"
unmount_esp
cmp -s "$root/Arrow-Native-candidate.kc" /Library/KernelCollections/AuxiliaryKernelExtensions.kc
sysctl -n kern.bootsessionuuid > "$root/prepared-boot"
sync
touch "$root/STABLE_PROFILE_ARMED_FOR_NEXT_BOOT"
committed=1
trap - EXIT
echo VERIFIED_PROFILE_COMMITTED_FOR_NEXT_BOOT_NO_RESTART
