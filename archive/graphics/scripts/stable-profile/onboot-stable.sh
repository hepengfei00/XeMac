#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
[ "$(id -u)" = 0 ]
root=$(cd "$(dirname "$0")" && pwd)
boot=$(sysctl -n kern.bootsessionuuid)
# The recovery configuration has no trial marker; it always remains a normal boot.
sysctl -n kern.bootargs | grep -q 'wtl-arrow-timing=45' || exit 0
mkdir -p "$root/boot-locks" "$root/boot-history"
mkdir "$root/boot-locks/$boot" 2>/dev/null || exit 0
exec >> "$root/stable-startup.log" 2>&1
printf 'BOOT=%s\n' "$boot"
if [ "$boot" = "$(cat "$root/prepared-boot")" ]; then echo ALREADY_VALIDATED_CURRENT_SESSION_NO_ACTION; exit 0; fi
archive=$(mktemp -d "$root/boot-history/previous.XXXXXX")
for name in started RESULT tested-boot result.log READY_FOR_REVIEW COMPOSITOR_SWITCHED display-audit.json; do
 [ ! -e "$root/$name" ] || mv "$root/$name" "$archive/$name"
done
completed=0
finish() {
 rc=$?
 trap - EXIT
 set +e
 if [ "$completed" = 0 ]; then
  bash "$root/restore.sh"
  printf 'FAILED_BOOT=%s NEXT_BOOT_ORIGINAL_CONFIGURATION\n' "$boot" >> "$root/stable-startup.log"
 fi
 exit "$rc"
}
trap finish EXIT
bash "$root/onboot-verified.sh"
bash "$root/health.sh"
bash "$root/commit-next-boot.sh"
completed=1
printf 'SUCCESSFUL_BOOT=%s NEXT_BOOT_VERIFIED_PROFILE_READY\n' "$boot"
trap - EXIT
