from pathlib import Path
import subprocess as s,re,json
r=Path(__file__).resolve().parents[2];f=Path(__file__).resolve().parent
stub=(f/'EarlyClaim.cpp').read_text();stub=stub[stub.index('static bool readGate'):stub.index('class WTLArrowEarlyClaim')]
core=(r/'core/gpu/xegfx/shim/src/xnu/xg_kext.cpp').read_text();a=core.index('static bool\nxg_trial_once_begin(void)');b=core.index('\n/* K4:',a);core=core[a:b]
mock=r'''
#define IOLog(...) ((void)0)
#include <cstdint>
#include <cstring>
#include <vector>
#include <map>
#include <string>
#include <memory>
#include <cassert>
struct OSObject {virtual ~OSObject()=default;virtual void release(){}};
std::vector<std::unique_ptr<OSObject>> objects;
struct OSData:OSObject {
 std::vector<uint8_t> b;
 static OSData *withBytes(const void*p,unsigned n){auto d=std::make_unique<OSData>();d->b.assign((const uint8_t*)p,(const uint8_t*)p+n);auto out=d.get();objects.push_back(std::move(d));return out;}
 unsigned getLength(){return b.size();} const void *getBytesNoCopy(){return b.data();}
};
#define OSDynamicCast(T,p) dynamic_cast<T*>(p)
#define OSSafeReleaseNULL(p) do{if(p)(p)->release();p=nullptr;}while(0)
struct IORegistryPlane{};
bool writeOK=true,corrupt=false,optionsPresent=true;uint32_t bootRevision=3;
static bool PE_parse_boot_argn(const char*,void*out,unsigned){memcpy(out,&bootRevision,4);return true;}
std::map<std::string,OSObject*> gates;
std::map<std::string,bool> resources;
struct IORegistryEntry:OSObject {
 static const IORegistryPlane *getPlane(const char*){static IORegistryPlane p;return &p;}
 static IORegistryEntry *fromPath(const char*,const IORegistryPlane*){static IORegistryEntry p;return optionsPresent?&p:nullptr;}
 OSObject *copyProperty(const char*k){auto it=gates.find(k);return it==gates.end()?nullptr:it->second;}
 bool setProperty(const char*k,OSObject*v){if(!writeOK)return false;if(corrupt){uint32_t one=1;gates[k]=OSData::withBytes(&one,4);}else gates[k]=v;return true;}
};
const IORegistryPlane *gIODTPlane=IORegistryEntry::getPlane("IODeviceTree");
bool kOSBooleanTrue=true;
struct IOService {static void publishResource(const char*k,bool v){resources[k]=v;}};
static bool xg_resource_flag(const char*k){return resources[k];}
#define IOLog(...) ((void)0)
void gate(const char*k,uint32_t v,unsigned n=4){gates[k]=OSData::withBytes(&v,n);}
void reset(){gates.clear();resources.clear();bootRevision=3;writeOK=true;corrupt=false;optionsPresent=true;gate("xegfx-disp-once",1);gate("xegfx-disp-phase",2);gate("xegfx-disp-trial",1);gate("xegfx-disp-fail",0);gate("xegfx-disp-external-dp",1);gate("xegfx-disp-kms",3);}
'''
tests=r'''
int main(){
 reset();bootRevision=2;assert(!armed());bootRevision=3;assert(armed());assert(consume());assert(!armed());
 resources["WTLArrowEarlyClaimActive"]=true;resources["WTLArrowTrialActive"]=true;
 assert(xg_trial_once_begin());resources.clear();assert(!xg_trial_once_begin());assert(!armed());
 const char *keys[]={"xegfx-disp-once","xegfx-disp-phase","xegfx-disp-trial","xegfx-disp-fail","xegfx-disp-external-dp","xegfx-disp-kms"};
 const uint32_t good[]={1,2,1,0,1,3};unsigned cases=0;
 for(unsigned i=0;i<6;i++)for(uint32_t v:{0u,1u,2u,3u,0xffffffffu}){reset();gate(keys[i],v);assert(armed()==(v==good[i]));cases++;}
 for(unsigned i=0;i<6;i++){reset();gates.erase(keys[i]);assert(!armed());reset();gate(keys[i],good[i],1);assert(!armed());cases+=2;}
 reset();writeOK=false;assert(!consume());assert(armed());assert(!xg_trial_once_begin());
 reset();corrupt=true;assert(!consume());assert(!xg_trial_once_begin());
 reset();gate("xegfx-disp-once",0);resources["WTLArrowEarlyClaimActive"]=true;assert(!xg_trial_once_begin());
 resources["WTLArrowTrialActive"]=true;gate("xegfx-disp-once",0,1);assert(!xg_trial_once_begin());
 reset();optionsPresent=false;assert(!armed());assert(!consume());assert(!xg_trial_once_begin());
 reset();gates.erase("xegfx-disp-once");assert(!armed());assert(xg_trial_once_begin());
 return cases==42?0:1;
}
'''
(f/'handshake-test.cpp').write_text(mock+stub+core+tests)
s.run(['/usr/bin/clang++','-std=c++17','-fsanitize=address,undefined','-g',str(f/'handshake-test.cpp'),'-o',str(f/'handshake-test')],check=True)
s.run([str(f/'handshake-test')],check=True)
assert 'shutdown' not in (f/'EarlyClaim.cpp').read_text()
assert (f/'EarlyClaim.cpp').read_text().index('if (!consume()) return false;') < (f/'EarlyClaim.cpp').read_text().index('IOService::publishResource("WTLArrowEarlyClaimActive"')
(f/'handshake-verdict.json').write_text(json.dumps({'gate_variations':42,'token_transfer_next_boot_write_failure_malformed_cases':'pass','sanitizers':['address','undefined'],'early_helper_has_no_reboot_or_display_mmio':True},indent=2)+'\n')
print('Early claim handshake and recovery checks: pass')
