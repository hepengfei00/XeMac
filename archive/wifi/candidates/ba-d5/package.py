from pathlib import Path
import shutil,plistlib,json,subprocess,hashlib,ctypes
p=Path(__file__).resolve().parent
base=p.parents[1]
previous=p.parent/'native-rx-fix'/'stage'
stage=p/'stage'
stage.mkdir(exist_ok=True)
kext=stage/'AirportRTW.kext'
if kext.exists(): raise SystemExit('Already packaged; inspect before replacing any package.')
shutil.copytree(p/'source/build/native-fix-release/Release/Tahoe/AirportRTW.kext',kext)
info=plistlib.loads((previous/'AirportRTW.kext/Contents/Info.plist').read_bytes())
info['CFBundleVersion']='2.3.1d5'
(kext/'Contents/Info.plist').write_bytes(plistlib.dumps(info,sort_keys=False))
# Preserve the proven single-device personality and AWDL-off configuration.
for name in ['ocvalidate','graphics-baseline.sha256','config.before.plist']:
 shutil.copy2(previous/name,stage/name)
old='WTLNativeRxTrial1';new='WTLNativeBATrial1'
config=plistlib.loads((previous/'config.wifi.plist').read_bytes())
changed=0
for entry in config['Kernel']['Add']:
 if old in entry['BundlePath']:
  entry['BundlePath']=entry['BundlePath'].replace(old,new);changed+=1
assert changed==1
(stage/'config.wifi.plist').write_bytes(plistlib.dumps(config,sort_keys=False))
for name in ['install.sh','restore.sh','restore-efi-only.wifi.sh','collect-onboot.sh','collector.plist']:
 text=(previous/name).read_text()
 text=text.replace('NativeRxTrial.1','NativeBATrial.1').replace(old,new)
 text=text.replace('wtl-native-rx1-once','wtl-native-ba1-once')
 if name=='install.sh':
  text=text.replace('A39079FC-AEA5-5A88-9382-879E66DBB153','D4A4E6EA-1C87-5A39-AE86-9D2D0BCD3802')
  text=text.replace('721AE88F-E9E1-3AEB-BE1E-0E23CB013985','B8E9ACA5-7189-3FF0-9B81-EBB19A8E59B5')
  # Verify the actual d4 package, not the new d5 trial directory.
  text=text.replace('$root/NativePowerTrial.1/','$root/NativeRxTrial.1/')
  text=text.replace('04ff7098bdb66e66d679e117b8ce86609d72d3a89322380614c777aa360a825b','45918773f07cbb3e328393bbd54776d309eb2241efacfd784ae8f7aa4b55fb47')
  text=text.replace('2.3.1d4','2.3.1d5')
 (stage/name).write_text(text)
 if name.endswith('.sh'): (stage/name).chmod(0o755)
subprocess.run(['codesign','--force','--sign','-','--timestamp=none',str(kext)],check=True,capture_output=True)
subprocess.run(['codesign','--verify','--deep','--strict',str(kext)],check=True)
subprocess.run([str(stage/'ocvalidate'),str(stage/'config.wifi.plist')],check=True)
for script in stage.glob('*.sh'):subprocess.run(['bash','-n',str(script)],check=True)
exe=kext/'Contents/MacOS/AirportRTW'
uuid=subprocess.check_output(['dwarfdump','--uuid',str(exe)],text=True).split()[1]
sha=hashlib.sha256(exe.read_bytes()).hexdigest()
io=ctypes.CDLL('/System/Library/Frameworks/IOKit.framework/IOKit')
parse=io.OSKextParseVersionString;parse.argtypes=[ctypes.c_char_p];parse.restype=ctypes.c_int64
version=parse(b'2.3.1d5');assert version>0
candidate={'identifier':'com.senmiko.AirportRTW','version':'2.3.1d5','uuid':uuid,'binary_sha256':sha,
 'pci_match':'10ec:8922','awdl':False,'base':'2.3.1d4 / B8E9ACA5-7189-3FF0-9B81-EBB19A8E59B5',
 'purpose':'passive BA negotiation stage diagnostics, no performance/crypto/protocol policy changes',
 'installed':False,'active':False,'persistent':False,'runtime_result':'not tested',
 'restart_authorization':'none for d5; d4 authorization already consumed','kernel_version_parser':version}
(p/'candidate.json').write_text(json.dumps(candidate,indent=2)+'\n')
(stage/'candidate.json').write_text(json.dumps(candidate,indent=2)+'\n')
lines=[]
for f in sorted(stage.rglob('*')):
 if f.is_file() and f.name!='SHA256SUMS':
  lines.append(hashlib.sha256(f.read_bytes()).hexdigest()+'  '+str(f.relative_to(stage)))
(stage/'SHA256SUMS').write_text('\n'.join(lines)+'\n')
print(json.dumps(candidate,indent=2))
