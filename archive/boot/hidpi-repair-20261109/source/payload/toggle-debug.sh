#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
[ "$(id -u)" = 0 ]
pkg=$(cd "$(dirname "$0")" && pwd)
root='/Library/Application Support/WTLArrowTiming.EXAMPLE'
debug_sha=bcf35ac7da731a921d1046920ff13226bb30419376d6f61d54cfe84498ab2c0b
quiet_sha=e418a776718712aa67fd4f5f97efce987e6fb0061dae03fce3f15de74b2808e1
fallback_sha=a5079a675e67f6c9fd5001353d5803d749d63efdfdb611240e0b350d69304d8e
hash() { shasum -a 256 "$1" | awk '{print $1}'; }
case "${1:-}" in
 on) desired="$pkg/config.debug.private.plist"; wanted=$debug_sha ;;
 off) desired="$pkg/config.quiet.private.plist"; wanted=$quiet_sha ;;
 *) echo 'Usage: sudo bash toggle-debug.sh on|off'; exit 2 ;;
esac
[ "$(hash "$desired")" = "$wanted" ]
[ "$(hash "$root/stable-config.plist")" = "$fallback_sha" ]
for f in "$desired" "$root/trial-config.plist" "$root/stable-config.plist"; do
 [ "$(plutil -extract ACPI.Quirks.RebaseRegions raw -o - "$f")" = true ]
done
current=$(hash "$root/trial-config.plist")
[ "$current" = "$debug_sha" ] || [ "$current" = "$quiet_sha" ]
. "$root/esp.sh"
mount_esp
trap unmount_esp EXIT
[ "$(hash "$esp_config")" = "$current" ]
if [ "$current" = "$wanted" ]; then echo REQUESTED_MODE_ALREADY_SAVED; exit 0; fi
backup=$(mktemp -d "$pkg/debug-switch.XXXXXX")
cp "$root/trial-config.plist" "$backup/daily.plist"
cp "$esp_config" "$backup/efi.plist"
changed=0
finish() {
 rc=$?
 trap - EXIT
 if [ "$rc" != 0 ] && [ "$changed" = 1 ]; then
  set +e
  cp "$backup/daily.plist" "$root/trial-config.plist"
  write_config "$backup/efi.plist"
  sync
 fi
 unmount_esp
 exit "$rc"
}
trap finish EXIT
changed=1
cp -p "$root/trial-config.plist" "$root/trial-config.plist.debug-switch"
cat "$desired" > "$root/trial-config.plist.debug-switch"
cmp -s "$desired" "$root/trial-config.plist.debug-switch"
mv -f "$root/trial-config.plist.debug-switch" "$root/trial-config.plist"
write_config "$desired"
[ "$(hash "$root/trial-config.plist")" = "$wanted" ]
[ "$(hash "$esp_config")" = "$wanted" ]
changed=0
echo "DEBUG_MODE_${1}_SAVED_REBASE_RETAINED_NO_RESTART"
