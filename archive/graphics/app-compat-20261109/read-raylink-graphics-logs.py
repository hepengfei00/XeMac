from pathlib import Path
import subprocess,re,json,concurrent.futures,shlex
p=Path(__file__).resolve().parent;s=(p.parent/'bluetooth-check/native-fw1/quality-check/control-path.txt').read_text().strip();ssh=['ssh','-4','-S',s,'-o','BatchMode=yes','-o','HostKeyAlias=hackintosh-desktop','hackintosh@192.0.2.123']
files=['raylink_client_2026-11-08.log','debug_0_2026-11-09.log','debug_0_2026-11-08.log']
def read(name):
 path='/Library/Application Support/RayLink/'+name
 r=subprocess.run(ssh+['tail -c 200000 '+shlex.quote(path)],capture_output=True,text=True,errors='replace',timeout=18)
 lines=[]
 for line in r.stdout.splitlines():
  if not re.search('metal|opengl|vulkan|videotoolbox|hardware|decod|renderer|unsupported|codec|create.*(texture|device|buffer)',line,re.I):continue
  if re.search('token|password|secret|authorization',line,re.I):continue
  line=re.sub(r'https?://\S+','<url>',line)
  line=re.sub(r'(?:\d{1,3}\.){3}\d{1,3}(?::\d+)?','<ip>',line)
  line=re.sub(r'(?:[0-9a-f]{2}[:-]){5}[0-9a-f]{2}','<address>',line,flags=re.I)
  line=re.sub(r'([\w]*(?:peer|user|device|session)[_-]?id\s*[:=]\s*)[^\s,;]+',r'\1<id>',line,flags=re.I)
  line=re.sub(r'\b\d{7,}\b','<large-number>',line)
  lines.append(line[:650])
 return {'source':name,'read_exit_status':r.returncode,'read_error':r.stderr[:160],'matching_lines_in_last200k':len(lines),'last_matches':lines[-25:]}
with concurrent.futures.ThreadPoolExecutor(max_workers=3) as e:results=list(e.map(read,files))
(p/'raylink-graphics-log-excerpts.json').write_text(json.dumps(results,indent=2,ensure_ascii=False)+'\n');print(json.dumps(results,indent=2,ensure_ascii=False))
