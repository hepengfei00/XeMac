from pathlib import Path
import shutil,json,hashlib,subprocess,tarfile,tempfile,datetime,shlex
p=Path(__file__).resolve().parent
stage=p/'deployment-stage'
if stage.exists():raise SystemExit('Existing deployment stage; inspect before changing it.')
shutil.copytree(p/'stage',stage)
url=json.loads((p/'receiver-connection.json').read_text())['url']
collector=r'''#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
root='/Library/Application Support/WTLArrowTiming.EXAMPLE'
trial="$root/NativeBATrial.1"
[ "$(sysctl -n kern.bootsessionuuid)" != "$(cat "$trial/prepared-boot")" ] || exit 0
job=/Library/LaunchDaemons/org.local.wtl-native-ba1-once.plist
[ ! -f "$job" ] || mv "$job" "$trial/collector-consumed.plist"
out='/Users/hackintosh/Library/Logs/NativeBATrial.1'
mkdir -p "$out"
chmod 750 "$out"
chown hackintosh:staff "$out"
exec >> "$out/boot.log" 2>&1
for step in $(seq 1 60); do
 snapshot="$out/scalar-latest.txt"
 {
  echo "BA_SAMPLE=$step"
  printf 'BOOT_UUID='; sysctl -n kern.bootsessionuuid
  printf 'CONSOLE='; stat -f '%Su' /dev/console
  kmutil showloaded --list-only -b com.senmiko.AirportRTW 2>/dev/null || true
  ioreg -r -c AirportRTW -l -d 1 -w0 | grep -E '"(RTWBA[A-Za-z0-9]+|RTWNativeRx[A-Za-z0-9]+|RTW89(RxBAAgreements|AmpduRxStartCount|AmpduRxStartFailCount|TxAmpduMask|TxDescAggregationEnabled|RxCCMPReplays|RxCCMPIntegrityFailures))" = (Yes|No|[0-9]+|"[^"]*")$' || true
  sysctl debug.xegfx_stage debug.xegfxaccel_boot debug.xegfxaccel_iop_xe_flips debug.xegfx_agdc_k5_why 2>/dev/null || true
  for process in ControlCenter SystemUIServer; do pgrep -x "$process" || true; done
  if [ "$(stat -f '%Su' /dev/console)" = hackintosh ]; then bash "$root/health.sh" || true; fi
 } > "$snapshot" 2>&1
 cat "$snapshot"
 curl -fsS --connect-timeout 2 --max-time 4 --data-binary "@$snapshot" '__LOCAL_RECEIVER_URL__' >/dev/null 2>&1 || true
 sleep 5
done
echo BA_COLLECTION_COMPLETE
chown -R hackintosh:staff "$out"
'''
(p/'collector-template.sh').write_text(collector)
(stage/'collect-onboot.sh').write_text(collector.replace('__LOCAL_RECEIVER_URL__',url))
runner=r'''#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
[ "$(id -u)" = 0 ]
stage=$(cd "$(dirname "$0")" && pwd)
[ "$(/usr/bin/plutil -extract authorized_count raw -o - "$stage/restart-authorization.json")" = 1 ]
[ "$(/usr/bin/plutil -extract consumed_count raw -o - "$stage/restart-authorization.json")" = 0 ]
bash "$stage/install.sh"
root='/Library/Application Support/WTLArrowTiming.EXAMPLE'
trial="$root/NativeBATrial.1"
[ ! -e "$trial/RESTART_REQUESTED" ]
cd "$trial"
shasum -a 256 --strict --quiet -c SHA256SUMS
cmp "$trial/config.wifi.plist" "$root/NativeBATrial.1/config.wifi.plist"
echo D5_INSTALLATION_AND_READBACK_PASSED_REQUESTING_SINGLE_AUTHORIZED_RESTART
sysctl -n kern.bootsessionuuid > "$trial/boot-before-restart"
date -u > "$trial/RESTART_REQUESTED"
sync
/sbin/shutdown -r now
'''
(stage/'run-authorized.sh').write_text(runner)
(stage/'run-as-user.sh').write_text('''#!/bin/bash
set -euo pipefail
export LC_ALL=C
stage=$(cd "$(dirname "$0")" && pwd)
printf '\\n请输入黑苹果管理员密码；核对通过后会使用已授权的 1 次重启。\\n'
sudo /bin/bash "$stage/run-authorized.sh"
''')
auth={'authorized_at':datetime.datetime.now(datetime.timezone.utc).isoformat(),'authorized_count':1,'consumed_count':0,'user_reply':'允许新增1次，已保存工作并保留网线','scope':'d5 installation/readback and one test reboot; preserve 4K','additional_restarts_authorized':False}
(p/'restart-authorization.json').write_text(json.dumps(auth,ensure_ascii=False,indent=2)+'\n')
shutil.copy2(p/'restart-authorization.json',stage/'restart-authorization.json')
for f in stage.glob('*.sh'):
 f.chmod(0o755);subprocess.run(['bash','-n',str(f)],check=True)
lines=[]
for f in sorted(stage.rglob('*')):
 if f.is_file() and f.name!='SHA256SUMS':lines.append(hashlib.sha256(f.read_bytes()).hexdigest()+'  '+str(f.relative_to(stage)))
(stage/'SHA256SUMS').write_text('\n'.join(lines)+'\n')
with tarfile.open(p/'deployment-stage.tar.gz','w:gz') as t:
 for f in stage.iterdir():t.add(f,arcname=f.name)
sock=Path(tempfile.mkdtemp(prefix='native-ba-auth.',dir='/private/tmp'))/'s'
(p/'control-path.txt').write_text(str(sock)+'\n')
print('Authorized one-boot package ready; no target writes yet.')
