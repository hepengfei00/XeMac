#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
[ "$(id -u)" = 0 ]
root=$(cd "$(dirname "$0")" && pwd)
[ "$(stat -f '%Su' /dev/console)" = hackintosh ]
uid=$(id -u hackintosh)
nonce=$(uuidgen)
boot=$(sysctl -n kern.bootsessionuuid)
earliest=$(date +%s)
mkdir -p "$root/live-display-audits"
request=$(mktemp -d "$root/live-display-audits/request.XXXXXX")
chown hackintosh:staff "$request"
chmod 700 "$request"
args=(--output "$request/result.json" --nonce "$nonce")
case "${1:-audit}" in
    audit) ;;
    select) args+=(--select-hidpi) ;;
    *) exit 2 ;;
esac
launchctl asuser "$uid" sudo -u hackintosh /usr/bin/open -n -a "$root/LiveDisplayAudit.app" --args "${args[@]}" > "$request/open.log" 2>&1 &
opener=$!
trap 'kill "$opener" 2>/dev/null || true' EXIT
for attempt in $(seq 1 30); do
    if [ -s "$request/result.json" ]; then
        bash "$root/validate-display.sh" "$request/result.json" "$nonce" "$boot" "$earliest"
        cp "$request/result.json" "$root/display-audit.json.new"
        chown root:wheel "$root/display-audit.json.new"
        chmod 644 "$root/display-audit.json.new"
        mv -f "$root/display-audit.json.new" "$root/display-audit.json"
        printf 'LIVE_DISPLAY_AUDIT=%s BOOT=%s\n' "$request/result.json" "$boot"
        exit 0
    fi
    sleep 1
done
echo LIVE_DISPLAY_APP_NO_FRESH_RESULT >&2
exit 1
