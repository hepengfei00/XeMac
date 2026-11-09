from pathlib import Path
import struct,re,json,subprocess,hashlib
p=Path(__file__).resolve().parent;blob=(p.parent/'runtime-switch/bluetoothd').read_bytes()
for i in range(struct.unpack_from('>I',blob,4)[0]):
 cpu,sub,o,size,a=struct.unpack_from('>IIIII',blob,8+20*i)
 if cpu==0x1000007:break
image=bytearray(blob[o:o+size]);original=bytes(image)
s=(p/'candidate-source/BrcmPatchRAM/BlueToolFixup.cpp').read_text()
def arr(name):return bytes(int(x,16) for x in re.findall('0x([0-9a-fA-F]+)',re.search(name+r'\[\]\s*=\s*\{([^}]+)',s).group(1)))
results=[]
for name,pos in [('Emp2',0x171140),('ScanReset',0x225213),('ScanStop',0x73fe43)]:
 f,r=arr('kRtl8922'+name+'Original'),arr('kRtl8922'+name+'Patched')
 assert len(f)==len(r) and image.count(f)==1 and image.count(r)==0 and image.find(f)==pos
 assert pos//4096==(pos+len(f)-1)//4096
 image[pos:pos+len(f)]=r;results.append({'name':name,'pattern_matches':1,'file_offset':hex(pos),'page_safe':True})
assert image[0x225218:0x22521a]==bytes.fromhex('31ff')
assert 0x22521c+struct.unpack_from('<b',image,0x22521b)[0]==0x225226
assert image[0x73fe49:0x73fe4b]==bytes.fromhex('31f6')
assert 0x73fe51+struct.unpack_from('<i',image,0x73fe4d)[0]==0x73ff26
assert [i for i,(a,b) in enumerate(zip(original,image)) if a!=b]==[i for i,(a,b) in enumerate(zip(original,image)) if any(pos<=i<pos+length for pos,length in [(0x171140,10),(0x225218,2),(0x73fe49,2)]) and a!=b]
(p/'bluetoothd-scan-offline').write_bytes(image)
commands=['lldb','-b','-o','target create --arch x86_64 '+str(p/'bluetoothd-scan-offline')]
for addr,end in [('0x100225218','0x10022522a'),('0x10073fe43','0x10073fe65')]:commands+=['-o','disassemble --start-address '+addr+' --end-address '+end]
commands+=['-o','quit'];r=subprocess.run(commands,capture_output=True,text=True,check=True);(p/'patched-disassembly.txt').write_text(r.stdout+r.stderr)
k=p/'candidate-source/build/Products/Release/BlueToolFixup.kext/Contents/MacOS/BlueToolFixup';baseline=p.parent/'release/BlueToolFixup.kext/Contents/MacOS/BlueToolFixup'
def imports(f):return set(subprocess.check_output(['nm','-uj',str(f)],text=True).splitlines())
assert imports(k)==imports(baseline)
report={'patterns':results,'imports_identical_to_loaded_official_2_7_2':len(imports(k)),'version':'2.7.2d2','uuid_output':subprocess.check_output(['dwarfdump','--uuid',str(k)],text=True).strip(),'binary_sha256':hashlib.sha256(k.read_bytes()).hexdigest(),'scope':'Explicit EMP2+scan flags, Darwin25.6, installer checks25G332 and known Realtek0BDA:8922; no disk patch of bluetoothd','limits':'Scan reset/stop callback error branches are suppressed per upstream same-build workaround. Does not prove actual scanning or pairing; other errors in these callbacks could also be hidden. This is the final bounded trial.'}
(p/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
