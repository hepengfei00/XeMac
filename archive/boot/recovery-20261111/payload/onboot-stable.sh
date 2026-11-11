#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
[ "$(id -u)" = 0 ]
root=$(cd "$(dirname "$0")" && pwd)
policy="$root/BootPolicy.1/manager.sh"
boot=$(sysctl -n kern.bootsessionuuid)
exec >> "$root/stable-startup.log" 2>&1
if ! sysctl -n kern.bootargs | grep -q 'wtl-arrow-timing=45'; then
    printf 'SAFE_BOOT=%s CHECK_BOUNDED_RECOVERY\n' "$boot"
    bash "$policy" recover
    exit 0
fi
mkdir -p "$root/boot-locks" "$root/boot-history"
mkdir "$root/boot-locks/$boot" 2>/dev/null || exit 0
printf 'BOOT=%s\n' "$boot"
completed=0
finish() {
    rc=$?
    trap - EXIT TERM INT HUP
    set +e
    if [ "$completed" = 0 ]; then
        bash "$policy" fail "BOOT_EXIT_$rc"
        printf 'FAILED_BOOT=%s EXIT=%s NEXT_BOOT_SAFE_RECOVERY\n' "$boot" "$rc"
        sync
        bash "$root/restore-boot.sh"
    fi
    exit "$rc"
}
trap finish EXIT
trap 'exit 143' TERM
trap 'exit 130' INT
trap 'exit 129' HUP
bash "$policy" begin
if [ -f "$root/BootstrapRebase.1/PENDING" ]; then bash "$root/BootstrapRebase.1/consume.sh"; fi
archive=$(mktemp -d "$root/boot-history/previous.XXXXXX")
for name in started RESULT tested-boot result.log READY_FOR_REVIEW COMPOSITOR_SWITCHED DESKTOP_SWITCH_SKIPPED display-audit.json current-stage.txt; do
    [ ! -e "$root/$name" ] || mv "$root/$name" "$archive/$name"
done
bash "$root/onboot-verified.sh"
bash "$root/health.sh"
bash "$root/commit-next-boot.sh"
bash "$policy" success
completed=1
printf 'SUCCESSFUL_BOOT=%s NEXT_BOOT_VERIFIED_PROFILE_READY\n' "$boot"
trap - EXIT TERM INT HUP
