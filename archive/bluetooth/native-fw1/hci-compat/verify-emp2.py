from pathlib import Path
import struct,json,re,hashlib,subprocess
p=Path(__file__).resolve().parent
binary=p.parent/'runtime-switch/bluetoothd'; blob=binary.read_bytes()
for i in range(struct.unpack_from('>I',blob,4)[0]):
 cpu,sub,offset,size,align=struct.unpack_from('>IIIII',blob,8+20*i)
 if cpu==0x1000007:break
else:raise SystemExit('x86_64 missing')
image=bytearray(blob[offset:offset+size])
source=(p/'candidate-source/BrcmPatchRAM/BlueToolFixup.cpp').read_text()
def array(name):
 text=re.search(name+r'\[\]\s*=\s*\{([^}]+)',source).group(1)
 return bytes(int(x,16) for x in re.findall(r'0x([0-9a-fA-F]+)',text))
f=array('kRtl8922Emp2Original');r=array('kRtl8922Emp2Patched')
assert len(f)==len(r)==10 and image.count(f)==1 and image.count(r)==0
pos=image.find(f);assert pos==0x171140 and pos//4096==(pos+len(f)-1)//4096
assert image[pos+9:pos+14]==bytes.fromhex('e83de30d00')
image[pos:pos+10]=r
assert image[pos:pos+2]==bytes.fromhex('31ff')
assert pos+7+struct.unpack_from('<i',image,pos+3)[0]==0x172aa9
assert pos+9+struct.unpack_from('<b',image,pos+8)[0]==0x17114e
assert image[0x17114e:0x171153]==bytes.fromhex('5b415e5dc3')
assert not any(x in source for x in ['kRtlScanReset','kRtlScanStop'])
# Local artifact copy only, never deployed as a system binary.
(p/'bluetoothd-emp2-offline').write_bytes(image)
out=subprocess.run(['lldb','-b','-o','target create --arch x86_64 '+str(p/'bluetoothd-emp2-offline'),'-o','disassemble --start-address 0x100171140 --end-address 0x100171153','-o','quit'],capture_output=True,text=True,check=True)
(p/'emp2-patched-disassembly.txt').write_text(out.stdout+out.stderr)
report={'original_target_sha256':hashlib.sha256(blob).hexdigest(),'pattern_matches':1,'changed_bytes_max':10,'page_boundary_safe':True,'callback_va':'0x100172aa9','resume_va':'0x10017114e','epilogue_preserved':True,'scan_patches_included':False,'scope':'25G332 x86_64, explicit boot flag, known0BDA:8922 current controller','limitation':'Handles local EMP2 reject path as success; only appropriate for observed unsupported-command condition. No pairing validation yet.'}
(p/'emp2-verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
