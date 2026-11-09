from pathlib import Path
import subprocess,json,re,concurrent.futures,datetime
p=Path(__file__).resolve().parent;s=(p/'control-path.txt').read_text().strip()
ssh=['ssh','-4','-S',s,'-o','BatchMode=yes','-o','HostKeyAlias=hackintosh-desktop','hackintosh@192.0.2.123']
def run(command):
 r=subprocess.run(ssh+[command],capture_output=True,text=True,timeout=35)
 if r.returncode:raise RuntimeError('Read command failed: '+r.stderr[:100])
 return r.stdout
boot=run('sysctl -n kern.boottime');sec=int(re.search(r'sec = (\d+)',boot).group(1))
# log show uses target local time. Derive it on the target, not from this host timezone.
start=run("date -r "+str(sec)+" '+%Y-%m-%d %H:%M:%S'").strip()
commands={
 'bluetooth':'/usr/sbin/system_profiler SPBluetoothDataType -json',
 'audio':'/usr/sbin/system_profiler SPAudioDataType -json',
 'events':"/usr/bin/log show --start '"+start+"' --style ndjson --info --predicate 'process == \"bluetoothd\" AND (eventMessage CONTAINS \"Bluetooth error - restarting\" OR eventMessage CONTAINS \"HCI initialization failed\" OR eventMessage CONTAINS \"Stack initialization complete\" OR eventMessage CONTAINS \"Scan stopped improperly\" OR eventMessage CONTAINS \"Reset scan in LE_Scan\" OR eventMessage CONTAINS \"SetEventMaskPage2\" OR eventMessage CONTAINS \"underrun\" OR eventMessage CONTAINS \"overrun\" OR eventMessage CONTAINS \"packet loss\" OR eventMessage CONTAINS \"command timeout\" OR eventMessage CONTAINS \"disconnect reason\")'",
 'persistence':"root='/Library/Application Support/WTLArrowTiming.EXAMPLE'; cat \"$root/result.log\" \"$root/stable-enabled.sha256\"; tail -7 \"$root/stable-startup.log\"; sysctl -n kern.bootsessionuuid; ps -p 161 -o pid=,etime=,pcpu=,rss="
}
with concurrent.futures.ThreadPoolExecutor(max_workers=4) as ex:
 f={k:ex.submit(run,v) for k,v in commands.items()};out={k:v.result() for k,v in f.items()}
def sanitize(obj):
 rows=[]
 def walk(x):
  if isinstance(x,dict):
   safe={}
   for key,value in x.items():
    if not isinstance(key,str) or key in ['_name','name'] or re.search('address|serial|uuid|name|source|manufacturer',key,re.I):continue
    if re.search('connected|paired|rssi|codec|sample.?rate|transport|major.?type|minor.?type|device.?type|device.?class|channels|bit.?depth|default.?output|default.?system|profile|services',key,re.I) and isinstance(value,(str,int,float,bool)):
     if not re.search(r'(?:[0-9a-f]{2}[:-]){5}[0-9a-f]{2}',str(value),re.I):safe[key]=value
   if safe:rows.append(safe)
   for v in x.values():walk(v)
  elif isinstance(x,list):
   for v in x:walk(v)
 walk(obj);return rows
for k in ['bluetooth','audio']:
 clean=sanitize(json.loads(out[k]));(p/(k+'-sanitized.json')).write_text(json.dumps(clean,indent=2,ensure_ascii=False)+'\n');print(k,json.dumps(clean,indent=2,ensure_ascii=False))
counts={};events=[]
for line in out['events'].splitlines():
 try:d=json.loads(line)
 except ValueError:continue
 msg=d.get('eventMessage','')
 if not msg:continue
 msg=re.sub(r'(?:[0-9a-f]{2}[:-]){5}[0-9a-f]{2}','<device-address>',msg,flags=re.I)
 events.append({'timestamp':d.get('timestamp'),'pid':d.get('processID'),'message':msg})
 for term in ['Bluetooth error - restarting','HCI initialization failed','Stack initialization complete','Scan stopped improperly','Reset scan in LE_Scan','SetEventMaskPage2','underrun','overrun','packet loss','command timeout','disconnect reason']:
  if term in msg:counts[term]=counts.get(term,0)+1
(p/'current-boot-events.json').write_text(json.dumps(events,indent=2,ensure_ascii=False)+'\n')
(p/'persistence-readback.log').write_text(out['persistence']);print('PERSISTENCE\n'+out['persistence'])
initial=p/'summary.json'
if initial.exists() and not (p/'summary-initial.json').exists():initial.rename(p/'summary-initial.json')
summary={'boot_start_local':start,'boot_epoch':sec,'collected_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'current_boot_diagnostic_counts':counts,'initial_profiler_path_error_corrected_without_new_login':True,'initial_last15m_counts_include_old_boot_and_must_not_be_used_for_current_health':True,'limits':'Observation only: no measured Bluetooth bandwidth, codec bitrate, latency, retransmission counters or long-duration/sleep-wake validation.'}
(p/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary,indent=2));print('BT_QUALITY_FOLLOWUP_COMPLETE')
