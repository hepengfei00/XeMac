#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
[ "$(id -u)" = 0 ]
root=$(cd "$(dirname "$0")" && pwd)
before=$(pgrep -x WindowServer | head -1)
[ -n "$before" ]
printf '%s\n' "$before" > "$root/windowserver-before.txt"
# Keep launchctl outside the synchronous boot pipeline. Its completion AND a
# replacement WindowServer must be observed; a timeout never counts as success.
launchctl kickstart -k system/com.apple.WindowServer > "$root/windowserver-kickstart.txt" 2>&1 &
switcher=$!
trap 'kill "$switcher" 2>/dev/null || true' EXIT
finished=0
for attempt in $(seq 1 45); do
    if ! kill -0 "$switcher" 2>/dev/null; then
        wait "$switcher"
        finished=1
        break
    fi
    sleep 1
done
if [ "$finished" = 0 ]; then
    printf 'DESKTOP_SWITCH_TIMEOUT BEFORE_PID=%s\n' "$before" >&2
    exit 1
fi
for attempt in $(seq 1 30); do
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
