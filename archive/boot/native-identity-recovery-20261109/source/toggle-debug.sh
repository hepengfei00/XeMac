#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
[ "$(id -u)" = 0 ]
pkg=$(cd "$(dirname "$0")" && pwd)
root='/Library/Application Support/WTLArrowTiming.EXAMPLE'
debug_sha=b68bdd2a54fce7b54103757ba37ab74e4cfc8d2eb20b69d53c01bf4fe1a8380c
quiet_sha=21730927c8c1422c9d6b6e4ed3ea4bece3d770b02d05ed893dc1bb784297dbec
fallback_sha=09409ec0484908eaf5f00d16d4538ee158bb41a633696483fbf13217c88ed923
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
