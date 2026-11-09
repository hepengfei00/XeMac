from pathlib import Path
import subprocess,json,re,concurrent.futures
p=Path(__file__).resolve().parent
sock=(p.parent/'bluetooth-check/native-fw1/quality-check/control-path.txt').read_text().strip()
ssh=['ssh','-4','-S',sock,'-o','BatchMode=yes','-o','ConnectTimeout=6','-o','HostKeyAlias=hackintosh-desktop','hackintosh@192.0.2.123']
paths=['/Users/hackintosh/Library/Logs/DiagnosticReports/QQLive-2026-11-08-151420.ips','/Users/hackintosh/Library/Logs/DiagnosticReports/RayLink-2026-11-08-233659.ips']
def read(path):
 r=subprocess.run(ssh+['cat '+"'"+path+"'"],capture_output=True,text=True,check=True,timeout=15)
 text=r.stdout;decoder=json.JSONDecoder();pos=0;objects=[]
 while pos<len(text):
  while pos<len(text) and text[pos].isspace():pos+=1
  if pos>=len(text):break
  obj,n=decoder.raw_decode(text,pos);objects.append(obj);pos=n
 body=objects[-1];head=objects[0];images=body.get('usedImages',[])
 def frames(items):
  out=[]
  for f in items[:35]:
   i=f.get('imageIndex',-1);image=images[i] if isinstance(i,int) and 0<=i<len(images) else {}
   out.append({'image':image.get('name'),'image_uuid':image.get('uuid'),'symbol':f.get('symbol'),'symbol_offset':f.get('symbolLocation'),'image_offset':f.get('imageOffset')})
  return out
 fault=body.get('faultingThread');threads=body.get('threads',[]);selected=[]
 for i,t in enumerate(threads):
  if t.get('triggered') or i==fault:selected.append({'thread_index':i,'queue':t.get('queue'),'frames':frames(t.get('frames',[]))})
 exception_info=json.dumps(body.get('asi',{}),ensure_ascii=False)
 exception_info=re.sub(r'https?://[^\s"\\]+','<url>',exception_info)
 exception_info=re.sub(r'/Users/hackintosh/\s"]+','/Users/hackintosh',exception_info)
 result={'source_report':Path(path).name,'timestamp':body.get('captureTime',head.get('timestamp')),'process':body.get('procName'),'app_version':head.get('app_version'),'app_build':head.get('build_version'),'os_version':body.get('osVersion'),'exception':body.get('exception'),'termination':body.get('termination'),'faulting_thread':fault,'faulting_threads':selected,'last_exception_backtrace':frames(body.get('lastExceptionBacktrace',[])),'application_specific_info':exception_info[:2500],'relevant_loaded_images':[{'name':x.get('name'),'uuid':x.get('uuid')} for x in images if re.search(r'XeMTL|NVMTL|vulkan|libanv|VideoToolbox|AppleGVA|Metal|CoreMedia|AVFoundation',x.get('name',''),re.I)]}
 (p/(Path(path).stem+'-sanitized.json')).write_text(json.dumps(result,indent=2,ensure_ascii=False)+'\n');return result
with concurrent.futures.ThreadPoolExecutor(max_workers=2) as ex:
 for result in ex.map(read,paths):print(json.dumps(result,indent=2,ensure_ascii=False))
