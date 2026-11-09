#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
[ "$(id -u)" = 0 ]
pkg=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$pkg/.." && pwd)
[ -f "$pkg/PENDING" ] || exit 0
hash() { shasum -a 256 "$1" | awk '{print $1}'; }
[ "$(hash "$root/trial-config.plist")" = b68bdd2a54fce7b54103757ba37ab74e4cfc8d2eb20b69d53c01bf4fe1a8380c ]
[ "$(hash "$root/stable-config.plist")" = 09409ec0484908eaf5f00d16d4538ee158bb41a633696483fbf13217c88ed923 ]
[ "$(hash "$pkg/bootstrap.private.plist")" = a275ba7a9bc1562cba89ec870de976de74f272660f413b973289d7c696e56d63 ]
. "$root/esp.sh"
mount_esp
trap unmount_esp EXIT
current=$(hash "$esp_config")
if [ "$current" = a275ba7a9bc1562cba89ec870de976de74f272660f413b973289d7c696e56d63 ]; then
 write_config "$root/stable-config.plist"
else
 [ "$current" = 09409ec0484908eaf5f00d16d4538ee158bb41a633696483fbf13217c88ed923 ]
fi
[ "$(hash "$esp_config")" = 09409ec0484908eaf5f00d16d4538ee158bb41a633696483fbf13217c88ed923 ]
[ ! -e "$pkg/CONSUMED" ]
mv "$pkg/PENDING" "$pkg/CONSUMED"
sync
echo BOOTSTRAP_GATES_CONSUMED_NEXT_BOOT_SAFE_FALLBACK_READY
