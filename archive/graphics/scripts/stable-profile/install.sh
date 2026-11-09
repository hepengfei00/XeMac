#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
[ "$(id -u)" = 0 ]
[ "${SUDO_USER:-}" = hackintosh ]
[ "$(sysctl -n kern.bootsessionuuid)" = BF0C9BB1-47BA-5B19-83CB-FDA44B4E17BE ]
root='/Library/Application Support/WTLArrowTiming.EXAMPLE'
src=$(cd "$(dirname "$0")" && pwd)
job=/Library/LaunchDaemons/org.local.wtl-arrow-native-stable.plist
[ ! -e "$job" ]
[ "$(shasum -a 256 "$root/onboot.sh" | awk '{print $1}')" = 611870bc8466c8350a6199c9a64b84e933a59d9315369ecc98b1b74c9817fde9 ]
work=$(mktemp -d "$root/stable-install.XXXXXX")
chmod 755 "$work"
printf '%s\n' "$work" > "$src/install-workdir.txt"
exec > >(tee -a "$work/install.log") 2>&1
for f in health.sh commit-next-boot.sh onboot-stable.sh onboot-verified.sh; do ditto --norsrc "$src/$f" "$work/$f"; done
(cd "$work" && shasum -c "$src/SHA256SUMS")
for f in health.sh commit-next-boot.sh onboot-stable.sh onboot-verified.sh; do
 bash -n "$work/$f"
 cp "$work/$f" "$root/$f"
 chown root:wheel "$root/$f"
 chmod 744 "$root/$f"
done
bash "$root/health.sh" > "$work/health-before.txt"
completed=0
finish() {
 rc=$?
 trap - EXIT
 set +e
 if [ "$completed" = 0 ]; then
  launchctl bootout system/org.local.wtl-arrow-native-stable >/dev/null 2>&1
  [ ! -e "$job" ] || mv "$job" "$work/aborted-job.plist"
  bash "$root/restore-boot.sh"
  while read -r key value; do nvram -d "$key" >/dev/null 2>&1; done < "$root/gates.txt"
  echo STABLE_INSTALL_STOPPED_CURRENT_DESKTOP_KEPT_ORIGINAL_NEXT_BOOT_RESTORED
 fi
 exit "$rc"
}
trap finish EXIT
cat > "$job" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>Label</key><string>org.local.wtl-arrow-native-stable</string>
<key>ProgramArguments</key><array><string>/bin/bash</string><string>$root/onboot-stable.sh</string></array>
<key>RunAtLoad</key><true/>
<key>StandardOutPath</key><string>$root/stable-launchd.log</string>
<key>StandardErrorPath</key><string>$root/stable-launchd.log</string>
</dict></plist>
PLIST
chown root:wheel "$job"
chmod 644 "$job"
plutil -lint "$job"
bash "$root/commit-next-boot.sh"
launchctl bootstrap system "$job"
sleep 2
launchctl print system/org.local.wtl-arrow-native-stable > "$work/launchd-state.txt"
grep -q 'last exit code = 0' "$work/launchd-state.txt"
bash "$root/health.sh" > "$work/health-after.txt"
[ "$(sysctl -n kern.bootsessionuuid)" = BF0C9BB1-47BA-5B19-83CB-FDA44B4E17BE ]
cp "$root/stable-enabled.sha256" "$work/boot-profile.sha256"
cat "$work/health-after.txt"
echo SAME_SUCCESSFUL_BOOT_DRIVERS_EFFECTS_AND_4K_HIDPI_PRESERVED
printf '机器未重启。已将这一版保存为后续启动配置，并保留失败时恢复原配置的入口。\n'
touch "$root/STABLE_INSTALL_COMPLETE"
completed=1
trap - EXIT
