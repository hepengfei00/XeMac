#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
[ "$(id -u)" = 0 ]
pkg=$(cd "$(dirname "$0")" && pwd)
root='/Library/Application Support/WTLArrowTiming.EXAMPLE'
debug_sha=e1c8fc66fd293746521941dab468b0a68a07dacd13b15389bbd2d69e6ff1634e
quiet_sha=ead26ddfb4e249dab2d85fb53a7918d73da58977d0b33cb2e719fcfc45a51dac
case "${1:-}" in
    on) desired="$pkg/config.debug.private.plist"; wanted=$debug_sha ;;
    off) desired="$pkg/config.quiet.private.plist"; wanted=$quiet_sha ;;
    *) echo 'Usage: sudo bash toggle-debug.sh on|off'; exit 2 ;;
esac
hash() { shasum -a 256 "$1" | awk '{print $1}'; }
current=$(hash "$root/trial-config.plist")
[ "$current" = "$quiet_sha" ] || [ "$current" = "$debug_sha" ]
[ "$(hash "$desired")" = "$wanted" ]
. "$root/esp.sh"
mount_esp
trap unmount_esp EXIT
current=$(hash "$esp_config")
[ "$current" = "$quiet_sha" ] || [ "$current" = "$debug_sha" ]
cp -p "$root/trial-config.plist" "$root/trial-config.plist.debug-switch"
cat "$desired" > "$root/trial-config.plist.debug-switch"
cmp -s "$desired" "$root/trial-config.plist.debug-switch"
mv -f "$root/trial-config.plist.debug-switch" "$root/trial-config.plist"
write_config "$desired"
[ "$(hash "$root/trial-config.plist")" = "$wanted" ]
[ "$(hash "$esp_config")" = "$wanted" ]
unmount_esp
trap - EXIT
echo "DEBUG_MODE_${1}_SAVED_FOR_NEXT_BOOT_NO_RESTART"
