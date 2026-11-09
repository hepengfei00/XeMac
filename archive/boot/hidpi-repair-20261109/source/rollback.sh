#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
[ "$(id -u)" = 0 ]
pkg=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$pkg/.." && pwd)
[ "$root" = '/Library/Application Support/WTLArrowTiming.EXAMPLE' ]
before="$pkg/before"
. "$root/esp.sh"
mount_esp
trap unmount_esp EXIT
# Restore only the identified original SSD, with the exact pre-install image.
write_config "$before/EFI/OC/config.plist"
write_collection "$before/AuxiliaryKernelExtensions.kc"
while IFS= read -r f; do cp -p "$before/root/$f" "$root/$f"; done < "$pkg/changed-root-files.txt"
rm -rf "$root/BootstrapRebase.1"
ditto --norsrc "$before/BootstrapRebase.1" "$root/BootstrapRebase.1"
rm -f "$root/validate-display.sh" "$root/probe-display.sh" "$root/switch-desktop.sh"
rm -rf "$root/LiveDisplayAudit.app"
unmount_esp
trap - EXIT
echo EXACT_PRE_REPAIR_PROFILE_AND_CACHE_RESTORED_NO_RESTART
