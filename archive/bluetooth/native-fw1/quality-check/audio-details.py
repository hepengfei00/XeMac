from pathlib import Path
import subprocess,json,re,concurrent.futures
p=Path(__file__).resolve().parent;s=(p/'control-path.txt').read_text().strip();ssh=['ssh','-4','-S',s,'-o','BatchMode=yes','-o','HostKeyAlias=hackintosh-desktop','hackintosh@192.0.2.123']
def read(kind):
 r=subprocess.run(ssh+['/usr/sbin/system_profiler '+kind+' -json'],capture_output=True,text=True,check=True,timeout=30);return json.loads(r.stdout)
def parse(obj):
 rows=[]
 def walk(x,context='unspecified'):
  if isinstance(x,dict):
   safe={}
   for key,val in x.items():
    if not isinstance(key,str) or key=='_name' or re.search('name|address|serial|uuid|source|manufacturer',key,re.I):continue
    if isinstance(val,(str,int,float,bool)) and (re.search('rssi|codec|major.?type|minor.?type|services|paired|srate|transport|bit.?depth|bps|coreaudio_device_(input|output)$|default',key,re.I)):
     if not re.search(r'(?:[0-9a-f]{2}[:-]){5}[0-9a-f]{2}',str(val),re.I):safe[key]=val
   if safe:safe['connection_section']=context;rows.append(safe)
   for key,value in x.items():
    child=context
    if isinstance(key,str) and re.match(r'^(device|devices)_(not_)?connected$',key):child=key
    walk(value,child)
  elif isinstance(x,list):
   for value in x:walk(value,context)
 walk(obj);return rows
with concurrent.futures.ThreadPoolExecutor(max_workers=2) as e:
 fs={k:e.submit(read,v) for k,v in [('bluetooth','SPBluetoothDataType'),('audio','SPAudioDataType')]};result={k:parse(v.result()) for k,v in fs.items()}
(p/'audio-connection-details.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n');print(json.dumps(result,ensure_ascii=False,indent=2))
