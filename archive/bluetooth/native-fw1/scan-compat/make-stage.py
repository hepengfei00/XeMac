from pathlib import Path
import shutil,plistlib,json,hashlib,subprocess,tarfile,difflib
p=Path(__file__).resolve().parent;prev=p.parent/'hci-compat';stage=p/'stage'
shutil.copytree(prev/'stage',stage,dirs_exist_ok=True)
shutil.rmtree(stage/'BlueToolFixup.kext')
shutil.copytree(p/'candidate-source/build/Products/Release/BlueToolFixup.kext',stage/'BlueToolFixup.kext')
for name in ['prepare.sh','restore-efi-only.bluetooth.sh','restore-known-d3.sh']:
 s=(prev/'stage'/name).read_text().replace('48294D9D-66CD-5962-BD0A-F9D88E3989CA','DF4A3FDB-2FDE-542A-A032-9043C421C78D').replace('BluetoothTrial.4','BluetoothTrial.5').replace('bluetooth-trial4','bluetooth-trial5').replace('WTLBluetoothTrial4','WTLBluetoothTrial5')
 if name=='restore-known-d3.sh':
  s=s.replace('"$trial/$key.before.plist"','"$root/BluetoothTrial.4/$key.before.plist"')
 (stage/name).write_text(s)
# Existing EMP2 callback can already be visible through the current code-page cache.
f=stage/'prepare.sh';s=f.read_text()
old='/usr/bin/perl -0777 -ne \'$p=pack("H*","488b3d596a9b0089dee8");$n=()=/\\Q$p\\E/g; print "EMP2_TARGET_PATTERN_MATCHES=$n\\n";exit($n==1?0:1)\' /usr/sbin/bluetoothd'
new='/usr/bin/perl -0777 -ne \'$p=pack("H*","488b3d596a9b0089dee8");$r=pack("H*","31ffe862190000eb05e8");$n=()=/\\Q$p\\E/g;$m=()=/\\Q$r\\E/g;print "EMP2_TARGET_ORIGINAL=$n PATCHED=$m\\n";exit(($n==1 && $m==0)||($n==0 && $m==1)?0:1)\' /usr/sbin/bluetoothd'
assert s.count(old)==1
f.write_text(s.replace(old,new))
# Verify scan byte patterns on actual target before protected writes.
f=stage/'prepare.sh';s=f.read_text();anchor='./ocvalidate config.bluetooth.plist'
audit='''/usr/bin/perl -0777 -ne '$p=pack("H*","e9b6feffff85ff740abe0300");$n=()=/\\Q$p\\E/g; print "SCAN_RESET_TARGET_MATCHES=$n\\n";exit($n==1?0:1)' /usr/sbin/bluetoothd
/usr/bin/perl -0777 -ne '$p=pack("H*","4189f64989fc85f60f84d5000000");$n=()=/\\Q$p\\E/g; print "SCAN_STOP_TARGET_MATCHES=$n\\n";exit($n==1?0:1)' /usr/sbin/bluetoothd
'''
assert s.count(anchor)==1;s=s.replace(anchor,audit+anchor);f.write_text(s)
config=plistlib.loads((prev/'stage/config.bluetooth.plist').read_bytes());guid='0BD28A97-C5B1-5AEA-9FA0-04916DD496E4'
config['NVRAM']['Add'][guid]['boot-args']+=' -btlfxrtl8922scan'
for e in config['Kernel']['Add']:
 if e['BundlePath']=='WTLBluetoothTrial4/BlueToolFixup.kext':
  e['BundlePath']='WTLBluetoothTrial5/BlueToolFixup.kext';e['Comment']='RTL8922A25G332 EMP2+scan718 final bounded trial2.7.2d2'
(stage/'config.bluetooth.plist').write_bytes(plistlib.dumps(config,sort_keys=False))
# Assert exact intended delta from installed EMP2 candidate.
check=plistlib.loads((stage/'config.bluetooth.plist').read_bytes());baseline=plistlib.loads((prev/'stage/config.bluetooth.plist').read_bytes())
check['NVRAM']['Add'][guid]['boot-args']=check['NVRAM']['Add'][guid]['boot-args'].removesuffix(' -btlfxrtl8922scan')
for e,old in zip(check['Kernel']['Add'],baseline['Kernel']['Add']):
 if e['BundlePath']=='WTLBluetoothTrial5/BlueToolFixup.kext':e.update(old)
assert check==baseline
for name in ['enabled-files.txt','collector.plist']:
 b=(prev/'stage'/name).read_bytes().replace(b'WTLBluetoothTrial4',b'WTLBluetoothTrial5').replace(b'BluetoothTrial.4',b'BluetoothTrial.5').replace(b'bluetooth-trial4',b'bluetooth-trial5');(stage/name).write_bytes(b)
template=(prev/'collector-template.sh').read_text().replace('BluetoothTrial.4','BluetoothTrial.5').replace('48294D9D-66CD-5962-BD0A-F9D88E3989CA','DF4A3FDB-2FDE-542A-A032-9043C421C78D').replace('BT_EMP2_TRIAL=1','BT_SCAN_TRIAL=1')
(p/'collector-template.sh').write_text(template)
url=json.loads((p.parent/'receiver-connection.json').read_text())['url'];assert "'" not in url
(stage/'collect-onboot.sh').write_text(template.replace('__ENDPOINT__',url))
for f in stage.glob('*.sh'):subprocess.run(['bash','-n',str(f)],check=True);f.chmod(0o755)
subprocess.run([str(stage/'ocvalidate'),str(stage/'config.bluetooth.plist')],check=True)
files=sorted(f for f in stage.rglob('*') if f.is_file() and f.name!='SHA256SUMS');(stage/'SHA256SUMS').write_text(''.join(hashlib.sha256(f.read_bytes()).hexdigest()+'  '+str(f.relative_to(stage))+'\n' for f in files))
with tarfile.open(p/'stage.tar.gz','w:gz') as t:
 for f in files+[stage/'SHA256SUMS']:t.add(f,arcname=str(f.relative_to(stage)),recursive=False)
a=(prev/'candidate-source/BrcmPatchRAM/BlueToolFixup.cpp').read_text().splitlines(True);b=(p/'candidate-source/BrcmPatchRAM/BlueToolFixup.cpp').read_text().splitlines(True)
(p/'0002-rtl8922a-scan718-final.patch').write_text(''.join(difflib.unified_diff(a,b,fromfile='a/BlueToolFixup.cpp',tofile='b/BlueToolFixup.cpp')))
print('Final-round stage and baseline-flag recovery prepared; target untouched.')
