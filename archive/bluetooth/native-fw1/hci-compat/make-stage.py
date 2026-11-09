from pathlib import Path
import shutil,plistlib,hashlib,json,subprocess,difflib
p=Path(__file__).resolve().parent; previous=p.parent/'usb-board-profile/stage';stage=p/'stage';stage.mkdir(exist_ok=True)
for name in ['config.d3.plist','ocvalidate']:
 shutil.copy2(previous/name,stage/name)
shutil.copytree(p/'candidate-source/build/Products/Release/BlueToolFixup.kext',stage/'BlueToolFixup.kext',dirs_exist_ok=True)
config=plistlib.loads((previous/'config.bluetooth.plist').read_bytes())
for entry in config['Kernel']['Add']:
 if entry['Enabled'] and entry['BundlePath'].endswith('/BlueToolFixup.kext'):
  entry['BundlePath']='WTLBluetoothTrial4/BlueToolFixup.kext';entry['Comment']='RTL8922A 25G332 EMP2-only trial BlueToolFixup2.7.2d1'
guid='0BD28A97-C5B1-5AEA-9FA0-04916DD496E4'
add=config['NVRAM']['Add'][guid];add['boot-args']+=' -btlfxrtl8922emp2'
flags={'bluetoothInternalControllerInfo':bytes(14),'bluetoothExternalDongleFailed':bytes(1),'bluetoothHostControllerSwitchBehavior':'always'}
add.update(flags)
delete=config['NVRAM']['Delete'].setdefault(guid,[])
for key in flags:
 if key not in delete:delete.append(key)
(stage/'config.bluetooth.plist').write_bytes(plistlib.dumps(config,sort_keys=False))
# Prove intended configuration delta is limited to the BlueTool entry, its flag and Bluetooth NVRAM.
check=plistlib.loads((stage/'config.bluetooth.plist').read_bytes());baseline=plistlib.loads((previous/'config.bluetooth.plist').read_bytes())
for key in flags:check['NVRAM']['Add'][guid].pop(key);check['NVRAM']['Delete'][guid].remove(key)
check['NVRAM']['Add'][guid]['boot-args']=check['NVRAM']['Add'][guid]['boot-args'].removesuffix(' -btlfxrtl8922emp2')
for e,old in zip(check['Kernel']['Add'],baseline['Kernel']['Add']):
 if e['BundlePath']=='WTLBluetoothTrial4/BlueToolFixup.kext':e.update(old)
assert check==baseline
prepare=(previous/'prepare.sh').read_text().replace('CDFC4E42-A2F2-5A4C-8B49-7B350C77E009','48294D9D-66CD-5962-BD0A-F9D88E3989CA').replace('BluetoothTrial.3','BluetoothTrial.4').replace('bluetooth-trial3','bluetooth-trial4')
prepare=prepare.replace('for name in BlueToolFixup RealtekBluetoothFirmware; do codesign --verify --deep --strict "$stage/$name.kext"; done','codesign --verify --deep --strict "$stage/BlueToolFixup.kext"\n[ "$(sw_vers -buildVersion)" = 25G332 ]\n[ "$(uname -m)" = x86_64 ]\n/usr/bin/perl -0777 -ne \'$p=pack("H*","488b3d596a9b0089dee8");$n=()=/\\Q$p\\E/g; print "EMP2_TARGET_PATTERN_MATCHES=$n\\n";exit($n==1?0:1)\' /usr/sbin/bluetoothd')
prepare=prepare.replace('dest="$esp_mp/EFI/OC/Kexts/WTLBluetoothTrial2"\n[ -d "$dest" ]','dest="$esp_mp/EFI/OC/Kexts/WTLBluetoothTrial4"\n[ ! -e "$dest" ]')
prepare=prepare.replace('du -sk "$stage/BlueToolFixup.kext" "$stage/RealtekBluetoothFirmware.kext"','du -sk "$stage/BlueToolFixup.kext"')
old='''for name in BlueToolFixup RealtekBluetoothFirmware; do
    diff -qr -x '._*' "$stage/$name.kext" "$dest/$name.kext"
    codesign --verify --deep --strict "$dest/$name.kext"
done'''
new='''mkdir -m 755 "$dest"
ditto --norsrc "$stage/BlueToolFixup.kext" "$dest/BlueToolFixup.kext"
diff -qr -x '._*' "$stage/BlueToolFixup.kext" "$dest/BlueToolFixup.kext"
codesign --verify --deep --strict "$dest/BlueToolFixup.kext"
for key in bluetoothInternalControllerInfo bluetoothExternalDongleFailed bluetoothHostControllerSwitchBehavior; do
    if ! nvram -x "$key" > "$trial/$key.before.plist" 2>/dev/null; then rm -f "$trial/$key.before.plist"; fi
done'''
assert old in prepare;prepare=prepare.replace(old,new)
prepare=prepare.replace('"$dest/RealtekBluetoothFirmware.kext/Contents/MacOS/RealtekBluetoothFirmware"','"$esp_mp/EFI/OC/Kexts/WTLBluetoothTrial2/RealtekBluetoothFirmware.kext/Contents/MacOS/RealtekBluetoothFirmware"')
# Check existing preserved Realtek identity rather than copying over it.
anchor='changed=1\ncp "$trial/restore-efi-only.bluetooth.sh"'
prepare=prepare.replace(anchor,'[ "$(sha "$esp_mp/EFI/OC/Kexts/WTLBluetoothTrial2/RealtekBluetoothFirmware.kext/Contents/MacOS/RealtekBluetoothFirmware")" = 01c25091c752575a16667f1a4c12fc65ab92decabb71f1963baca2c4e9be1b16 ]\n'+anchor)
(stage/'prepare.sh').write_text(prepare)
for name in ['restore-efi-only.bluetooth.sh','restore-known-d3.sh']:
 content=(previous/name).read_text().replace('BluetoothTrial.3','BluetoothTrial.4');(stage/name).write_text(content)
collector=plistlib.loads((previous/'collector.plist').read_bytes())
def replace(obj):
 if isinstance(obj,str):return obj.replace('BluetoothTrial.3','BluetoothTrial.4').replace('bluetooth-trial3','bluetooth-trial4')
 if isinstance(obj,list):return [replace(x) for x in obj]
 if isinstance(obj,dict):return {k:replace(v) for k,v in obj.items()}
 return obj
(stage/'collector.plist').write_bytes(plistlib.dumps(replace(collector)))
files=[]
for entry in config['Kernel']['Add']:
 if not entry['Enabled']:continue
 for key,kind in [('ExecutablePath','binary'),('PlistPath','plist')]:
  if entry.get(key):files.append(kind+'|Kexts/'+entry['BundlePath']+'/'+entry[key])
for section in ['Drivers','Tools']:
 for entry in config['UEFI' if section=='Drivers' else 'Misc'][section]:
  if entry['Enabled']:files.append('binary|'+section+'/'+entry['Path'])
for entry in config['ACPI']['Add']:
 if entry['Enabled']:files.append('file|ACPI/'+entry['Path'])
files.append('file|OpenCore.efi')
assert len(files)==29
(stage/'enabled-files.txt').write_text('\n'.join(files)+'\n')
for f in stage.glob('*.sh'):subprocess.run(['bash','-n',str(f)],check=True)
subprocess.run([str(stage/'ocvalidate'),str(stage/'config.bluetooth.plist')],check=True)
source=p/'candidate-source/BrcmPatchRAM/BlueToolFixup.cpp';original=p/'source-unmodified/BrcmPatchRAM-2.7.2/BrcmPatchRAM/BlueToolFixup.cpp'
(p/'0001-rtl8922a-25g332-emp2-only.patch').write_text(''.join(difflib.unified_diff(original.read_text().splitlines(True),source.read_text().splitlines(True),fromfile='a/BrcmPatchRAM/BlueToolFixup.cpp',tofile='b/BrcmPatchRAM/BlueToolFixup.cpp')))
print('Stage prepared; enabled files:',len(files),'(target EFI contents still require root readback).')
