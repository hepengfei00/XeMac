from pathlib import Path
import subprocess,concurrent.futures,json,re,datetime
p=Path(__file__).resolve().parent;s=(p/'control-path.txt').read_text().strip()
ssh=['ssh','-4','-S',s,'-o','BatchMode=yes','-o','ConnectTimeout=6','-o','HostKeyAlias=hackintosh-desktop','hackintosh@192.0.2.123']
def run(cmd,timeout=50):
 r=subprocess.run(ssh+[cmd],capture_output=True,text=True,timeout=timeout)
 return {'exit':r.returncode,'out':r.stdout,'error':r.stderr}
commands={
 'identity':"sysctl -n kern.bootsessionuuid; sysctl -n kern.bootargs; kmutil showloaded --list-only 2>/dev/null | grep -E 'BlueToolFixup|RealtekBluetoothFirmware|AirportRTW|XeGfxDisplay|WTLArrowEarlyClaim'; /bin/bash '/Library/Application Support/WTLArrowTiming.EXAMPLE/health.sh'; shasum -a 256 '/Library/Application Support/WTLArrowTiming.EXAMPLE/trial-config.plist' '/Library/Application Support/WTLArrowTiming.EXAMPLE/restore-efi-only.sh'; ps -axo pid=,comm= | awk '$2==\"/usr/sbin/bluetoothd\" {print \"CURRENT_BT_PID=\"$1}'",
 'cpu':"for n in 1 2 3 4 5 6; do date -u '+SAMPLE %H:%M:%S'; ps -axo pid=,pcpu=,pmem=,rss=,etime=,comm= | awk '$6 ~ /(^|\\/)(bluetoothd|bluetoothaudiod|bluetoothuserd|coreaudiod)$/'; sleep 5; done",
 'bluetooth_json':'/usr/bin/system_profiler SPBluetoothDataType -json',
 'audio_json':'/usr/bin/system_profiler SPAudioDataType -json',
 'errors':"/usr/bin/log show --last 15m --style ndjson --info --predicate 'process == \"bluetoothd\" AND (eventMessage CONTAINS \"Bluetooth error - restarting\" OR eventMessage CONTAINS \"HCI initialization failed\" OR eventMessage CONTAINS \"Stack initialization complete\" OR eventMessage CONTAINS \"Scan stopped improperly\" OR eventMessage CONTAINS \"Reset scan in LE_Scan\" OR eventMessage CONTAINS \"SetEventMaskPage2\" OR eventMessage CONTAINS \"underrun\" OR eventMessage CONTAINS \"overrun\" OR eventMessage CONTAINS \"packet loss\" OR eventMessage CONTAINS \"command timeout\" OR eventMessage CONTAINS \"disconnect reason\")'"
}
with concurrent.futures.ThreadPoolExecutor(max_workers=5) as ex:
 futures={k:ex.submit(run,v) for k,v in commands.items()};results={k:f.result() for k,f in futures.items()}
for k in ['identity','cpu']:
 (p/(k+'.log')).write_text(results[k]['out']+results[k]['error'])
 print('=== '+k.upper()+' ===\n'+results[k]['out']+results[k]['error'],flush=True)
# Never save device names/addresses or complete profiler JSON.
def select(obj,audio=False):
 out=[]
 def walk(x):
  if isinstance(x,dict):
   safe={}
   for key,value in x.items():
    if not isinstance(key,str) or key in ['_name','name'] or re.search('address|serial|uuid|name|source|manufacturer',key,re.I):continue
    if re.search('connected|rssi|codec|sample.?rate|transport|major.?type|minor.?type|device.?type|device.?class|channels|bit.?depth|default.?output|default.?system|profile|services',key,re.I) and isinstance(value,(str,int,float,bool)):
     if not re.search(r'(?:[0-9a-f]{2}[:-]){5}[0-9a-f]{2}',str(value),re.I):safe[key]=value
   if safe:out.append(safe)
   for value in x.values():walk(value)
  elif isinstance(x,list):
   for value in x:walk(value)
 walk(obj);return out
for key in ['bluetooth_json','audio_json']:
 r=results[key]
 try:clean=select(json.loads(r['out']),key=='audio_json')
 except Exception as e:clean={'parse_error':type(e).__name__,'exit':r['exit']}
 (p/(key.replace('_json','')+'-sanitized.json')).write_text(json.dumps(clean,indent=2,ensure_ascii=False)+'\n');print('=== '+key+' sanitized ===\n'+json.dumps(clean,indent=2,ensure_ascii=False),flush=True)
lines=[];counts={};pids=set()
for line in results['errors']['out'].splitlines():
 try:d=json.loads(line)
 except ValueError:continue
 message=d.get('eventMessage','');ts=d.get('timestamp','');pid=d.get('processID')
 if not message:continue
 # Only retain explicit selected diagnostics, never device identity.
 if not any(x in message for x in ['Bluetooth error - restarting','HCI initialization failed','Stack initialization complete','Scan stopped improperly','Reset scan in LE_Scan','SetEventMaskPage2','underrun','overrun','packet loss','command timeout','disconnect reason']):continue
 message=re.sub(r'(?:[0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}','<device-address>',message)
 lines.append({'timestamp':ts,'pid':pid,'message':message})
 if pid:pids.add(pid)
 for term in ['Bluetooth error - restarting','HCI initialization failed','Stack initialization complete','Scan stopped improperly','Reset scan in LE_Scan','SetEventMaskPage2','underrun','overrun','packet loss','command timeout','disconnect reason']:
  if term in message:counts[term]=counts.get(term,0)+1
(p/'diagnostic-events.json').write_text(json.dumps(lines,indent=2,ensure_ascii=False)+'\n')
summary={'collected_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'query_window':'last15m (includes previous boot if within window; correlate to current boot timestamp/PID)','diagnostic_counts':counts,'pids_in_diagnostics':sorted(pids),'command_exit_status':{k:v['exit'] for k,v in results.items()},'limits':'Read-only observation; no active Bluetooth bandwidth, packet-retransmission or latency benchmark. Missing RSSI/codec means unavailable, not zero. Names/addresses/full profiler JSON not saved.'}
(p/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary,indent=2),flush=True)
print('BT_QUALITY_READONLY_COMPLETE',flush=True)
