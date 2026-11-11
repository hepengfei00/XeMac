#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
[ "$(id -u)" = 0 ]
pkg=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$pkg/.." && pwd)
[ "$root" = '/Library/Application Support/WTLArrowTiming.li8Pgu' ]
STATE_FILE="$pkg/state.plist"
. "$pkg/state-machine.sh"
boot=$(sysctl -n kern.bootsessionuuid)
lock="$pkg/lock"
if ! mkdir "$lock" 2>/dev/null; then
    saved_boot=$(cat "$lock/boot" 2>/dev/null || true)
    saved_pid=$(cat "$lock/pid" 2>/dev/null || true)
    if [ -z "$saved_boot" ] || ! [[ "$saved_pid" =~ ^[0-9]+$ ]]; then
        boot_epoch=$(sysctl -n kern.boottime | awk '{print $4}' | tr -d ',')
        if [ "$(stat -f '%m' "$lock")" -ge "$boot_epoch" ]; then echo POLICY_BUSY_INITIALIZING; exit 2; fi
    fi
    if [ "$saved_boot" = "$boot" ] && [[ "$saved_pid" =~ ^[0-9]+$ ]] && kill -0 "$saved_pid" 2>/dev/null; then
        echo POLICY_BUSY; exit 2
    fi
    [ ! -L "$lock" ] && [ "$(stat -f '%u' "$lock")" = 0 ]
    mv "$lock" "$pkg/stale-lock.$(uuidgen)"
    mkdir "$lock"
fi
printf '%s\n' "$boot" > "$lock/boot"
printf '%s\n' "$$" > "$lock/pid"
trap 'rm -f "$lock/boot" "$lock/pid"; rmdir "$lock"' EXIT
event() { printf '%s BOOT=%s %s\n' "$(date -u +%FT%TZ)" "$boot" "$*" >> "$pkg/events.log"; }
hash() { shasum -a 256 "$1" | awk '{print $1}'; }
arm_reserved_retry() {
    . "$root/esp.sh"
    POLICY_TRANSACTION_DONE=0
    arm_cleanup() {
        cleanup_rc=$?
        trap - EXIT
        set +e
        if [ "$POLICY_TRANSACTION_DONE" != 1 ]; then
            sm_fail "$boot" ARM_PREPARATION_FAILED
            if [ -n "${esp_config:-}" ] && [ -f "$esp_config" ]; then
                case "$(hash "$esp_config")" in
                    "$(hash "$root/trial-config.plist")"|fe5c0b54c6729a9d385c07c890a1d6e4203546aee6f58926ea90d3a32a43f7a7)
                        write_config "$root/stable-config.plist"
                        write_collection "$root/AuxiliaryKernelExtensions-before.kc"
                        nvram 'xegfx-disp-once=%00%00%00%00'
                        ;;
                esac
            fi
            event ARM_FAILED
        fi
        unmount_esp
        rm -f "$lock/boot" "$lock/pid"; rmdir "$lock"
        exit "$cleanup_rc"
    }
    trap arm_cleanup EXIT
    trap 'exit 143' TERM
    trap 'exit 130' INT
    trap 'exit 129' HUP
    mount_esp
    if ! cmp -s "$esp_config" "$root/stable-config.plist"; then cmp -s "$esp_config" "$root/trial-config.plist"; fi
    (cd "$esp_mp/EFI/OC" && shasum -c "$pkg/enabled-files.sha256") > "$pkg/efi-check-last.log" 2>&1
    write_collection "$root/Arrow-Native-candidate.kc"
    nvram -x -f "$root/gates.plist"
    "$root/gate-audit" > "$pkg/armed-gates-last.txt"
    # EFI is the last hardware-facing write; prior interruption remains a safe boot.
    write_config "$root/trial-config.plist"
    cmp -s "$esp_config" "$root/trial-config.plist"
    shasum -a 256 "$esp_config" /Library/KernelCollections/AuxiliaryKernelExtensions.kc > "$pkg/next-boot-last.sha256"
    sm_armed "$boot"
    POLICY_TRANSACTION_DONE=1
    unmount_esp
    event NEXT_NATIVE_BOOT_PREPARED_CREDIT_SPENT_NO_RESTART
}
case "${1:-}" in
    begin) sm_begin "$boot"; event NATIVE_BEGIN;;
    stage) sm_stage "$boot" "$2"; event "STAGE_$2";;
    fail) sm_fail "$boot" "$2"; event "FAILED_$2";;
    success) sm_success "$boot"; event HEALTH_COMMITTED_RETRY_CREDIT_RESET;;
    recover|manual)
        # Never try to repair the live display by arming another GPU session.
        if sysctl -n kern.bootargs | grep -q 'wtl-arrow-timing=45'; then
            echo CURRENT_NATIVE_BOOT_USE_HEALTH_OR_SAFE_RECOVERY; exit 2
        fi
        if [ "$1" = recover ]; then
            nvram '4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:boot-path' | grep -iq 00000000-0000-0000-0000-000000000001 || exit 0
        fi
        sm_read
        # Calling a validator in an if-condition would suppress its set-e checks.
        # Run all checks in a separate strict shell before reserving the one retry.
        if ! /bin/bash "$pkg/check-payload.sh"; then
            sm_pause "$boot" PAYLOAD_OR_PROFILE_CHANGED
            event PAYLOAD_GUARD_REJECTED
            exit 1
        fi
        if [ "$1" = manual ]; then sm_manual "$boot"; fi
        reserve_rc=0
        sm_reserve "$boot" || reserve_rc=$?
        case "$reserve_rc" in
            0) event RETRY_RESERVED; arm_reserved_retry;;
            2) event ALREADY_PREPARED_OR_RUNNING_NO_EXTRA_CREDIT; exit 0;;
            3) event PAUSED_NO_AUTOMATIC_RETRY; exit 0;;
            *) exit "$reserve_rc";;
        esac
        ;;
    *) echo 'Usage: manager.sh begin|stage NAME|fail REASON|success|recover|manual'; exit 2;;
esac
