#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
[ "$(id -u)" = 0 ]
root=$(cd "$(dirname "$0")" && pwd)
before=$(pgrep -x WindowServer | head -1)
[ -n "$before" ]
printf '%s\n' "$before" > "$root/windowserver-before.txt"
heartbeat="$root/BootPolicy.1/switch-heartbeat.log"
printf '%s START BOOT=%s BEFORE_PID=%s\n' "$(date -u +%FT%TZ)" "$(sysctl -n kern.bootsessionuuid)" "$before" >> "$heartbeat"
sync
switcher=
finish() {
    rc=$?
    trap - EXIT TERM INT HUP
    set +e
    printf '%s EXIT=%s\n' "$(date -u +%FT%TZ)" "$rc" >> "$heartbeat"
    [ -z "$switcher" ] || kill "$switcher" 2>/dev/null
    sync
    exit "$rc"
}
trap finish EXIT
trap 'exit 143' TERM
trap 'exit 130' INT
trap 'exit 129' HUP
launchctl kickstart -k system/com.apple.WindowServer > "$root/windowserver-kickstart.txt" 2>&1 &
switcher=$!
finished=0
for ((attempt=0; attempt<45; attempt++)); do
    if ((attempt % 5 == 0)); then
        printf '%s WAIT_COMMAND tick=%s helper_pid=%s\n' "$(date -u +%FT%TZ)" "$attempt" "$switcher" >> "$heartbeat"
        sync
    fi
    if ! kill -0 "$switcher" 2>/dev/null; then
        switch_rc=0
        wait "$switcher" || switch_rc=$?
        switcher=
        [ "$switch_rc" = 0 ] || exit "$switch_rc"
        finished=1
        break
    fi
    sleep 1
done
if [ "$finished" = 0 ]; then echo DESKTOP_SWITCH_TIMEOUT >&2; exit 1; fi
for ((attempt=0; attempt<30; attempt++)); do
    after=$(pgrep -x WindowServer | head -1 || true)
    if [ -n "$after" ] && [ "$after" != "$before" ]; then
        sleep 3
        [ "$(pgrep -x WindowServer | head -1)" = "$after" ] || continue
        printf '%s\n' "$after" > "$root/windowserver-after-switch.txt"
        printf 'DESKTOP_SWITCH_RETURNED NEW_PID=%s\n' "$after"
        exit 0
    fi
    sleep 1
done
echo DESKTOP_REPLACEMENT_NOT_OBSERVED >&2
exit 1
