#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
[ "$(id -u)" = 0 ]
pkg=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$pkg/.." && pwd)
[ -f "$pkg/PENDING" ] || exit 0
hash() { shasum -a 256 "$1" | awk '{print $1}'; }
[ "$(hash "$root/trial-config.plist")" = bcf35ac7da731a921d1046920ff13226bb30419376d6f61d54cfe84498ab2c0b ]
[ "$(hash "$root/stable-config.plist")" = a5079a675e67f6c9fd5001353d5803d749d63efdfdb611240e0b350d69304d8e ]
[ "$(hash "$pkg/bootstrap.private.plist")" = b351ef49dcbf6733f5325b958534a1a3e5854383c238e617a8007f5797029c72 ]
. "$root/esp.sh"
mount_esp
trap unmount_esp EXIT
current=$(hash "$esp_config")
if [ "$current" = b351ef49dcbf6733f5325b958534a1a3e5854383c238e617a8007f5797029c72 ]; then
 write_config "$root/stable-config.plist"
else
 [ "$current" = a5079a675e67f6c9fd5001353d5803d749d63efdfdb611240e0b350d69304d8e ]
fi
[ "$(hash "$esp_config")" = a5079a675e67f6c9fd5001353d5803d749d63efdfdb611240e0b350d69304d8e ]
[ ! -e "$pkg/CONSUMED" ]
mv "$pkg/PENDING" "$pkg/CONSUMED"
sync
echo BOOTSTRAP_GATES_CONSUMED_NEXT_BOOT_SAFE_FALLBACK_READY
