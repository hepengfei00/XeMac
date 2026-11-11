#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
state='/Library/Application Support/WTLArrowTiming.li8Pgu/BootPolicy.1/state.plist'
docs='/Users/hackintosh/Documents/黑苹果启动管理'
[ -f "$state" ] || exit 0
phase=$(plutil -extract phase raw -o - "$state")
owner=$(plutil -extract owner_boot raw -o - "$state")
boot=$(sysctl -n kern.bootsessionuuid)
case "$phase" in
    healthy) message='最近一次核显启动已通过实时 4K HiDPI 检查。';;
    armed) message='已准备下一次开机重试 4K。请保存工作后正常重启，无需立即重启。';;
    paused) message='核显自动恢复已暂停，安全显示仍可用。可运行本目录的“重试4K启动.command”手动安排一次重试。';;
    failed) message='核显启动未完成。下一次安全启动将检查一次自动重试机会。';;
    *) message='正在处理核显启动，请保留当前现场。';;
esac
mkdir -p "$docs"
printf '%s\n状态：%s\n本次启动：%s\n记录所属启动：%s\n' "$message" "$phase" "$boot" "$owner" > "$docs/状态.txt"
case "$phase" in armed|paused|failed) ;; *) exit 0;; esac
[ "$owner" = "$boot" ] || exit 0
cache='/Users/hackintosh/Library/Caches/WTLBootPolicy'
mkdir -p "$cache"
stamp="$boot:$phase"
[ "$stamp" != "$(cat "$cache/notified" 2>/dev/null || true)" ] || exit 0
printf '%s\n' "$stamp" > "$cache/notified"
# Notification delivery is best effort; the status file remains available.
osascript -e 'on run argv' -e 'display notification (item 1 of argv) with title "4K 启动状态"' -e 'end run' "$message" >/dev/null 2>&1 || true
