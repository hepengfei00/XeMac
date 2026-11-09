from pathlib import Path
import subprocess,shlex,json
p=Path(__file__).resolve().parent
sock=(p/'control-path.txt').read_text().strip()
ssh=['ssh','-4','-T','-S',sock,'-o','ControlMaster=no','-o','BatchMode=yes','-o','ConnectTimeout=8','-o','HostKeyAlias=hackintosh-desktop','hackintosh@192.0.2.123']
url=json.loads((p/'receiver-connection.json').read_text())['url']
probe=subprocess.check_output(ssh+['curl -fsS --connect-timeout 3 --max-time 6 '+shlex.quote(url)],text=True,timeout=12)
assert probe=='BA_RECEIVER_READY','automatic readback receiver unreachable; no target configuration changed'
remote=subprocess.check_output(ssh+['mktemp -d /Users/hackintosh/Library/Logs/WTLNativeBAPrep.XXXXXX'],text=True,timeout=12).strip()
assert remote.startswith('/Users/hackintosh/Library/Logs/WTLNativeBAPrep.')
with (p/'deployment-stage.tar.gz').open('rb') as f:subprocess.run(ssh+['tar -xzf - -C '+shlex.quote(remote)],stdin=f,check=True,timeout=60)
subprocess.run(ssh+['cd '+shlex.quote(remote)+' && export LC_ALL=C && shasum -a 256 --strict --quiet -c SHA256SUMS && codesign --verify --deep --strict AirportRTW.kext'],check=True,timeout=25)
(p/'remote-deployment-path.txt').write_text(remote+'\n')
print('DEPLOYMENT_TRANSFER_READBACK_AND_AUTOMATIC_RESULT_PATH_PASSED')
