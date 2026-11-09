
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
static bool readGate(IORegistryEntry *options, const char *name, uint32_t *out) {
    OSObject *obj = options->copyProperty(name);
    OSData *data = OSDynamicCast(OSData, obj);
    bool valid = data && data->getLength() == sizeof(*out);
    if (valid) memcpy(out, data->getBytesNoCopy(), sizeof(*out));
    if (obj) obj->release();
    return valid;
}
static bool armed() {
    uint32_t bootRevision=0;
    if (!PE_parse_boot_argn("wtl-arrow-native",&bootRevision,sizeof(bootRevision)) || bootRevision!=3) return false;
    const IORegistryPlane *plane = IORegistryEntry::getPlane("IODeviceTree");
    if (!plane) return false;
    IORegistryEntry *options = IORegistryEntry::fromPath("/options", plane);
    if (!options) return false;
    uint32_t once=0, phase=0, trial=0, fail=1, external=0, kms=0;
    unsigned present = 0;
    if (readGate(options,"xegfx-disp-once",&once)) present |= 1;
    if (readGate(options,"xegfx-disp-phase",&phase)) present |= 2;
    if (readGate(options,"xegfx-disp-trial",&trial)) present |= 4;
    if (readGate(options,"xegfx-disp-fail",&fail)) present |= 8;
    if (readGate(options,"xegfx-disp-external-dp",&external)) present |= 16;
    if (readGate(options,"xegfx-disp-kms",&kms)) present |= 32;
    options->release();
    const bool valid = present == 63 && once==1 && phase==2 && trial==1 && fail==0 && external==1 && kms==3;
    IOLog("WTLArrowEarlyClaim: gate snapshot revision=%u present=0x%x once=%u phase=%u trial=%u fail=%u external=%u kms=%u armed=%u\n",
          bootRevision, present, once, phase, trial, fail, external, kms, valid);
    return valid;
}
static bool consume() {
    const IORegistryPlane *plane = IORegistryEntry::getPlane("IODeviceTree");
    IORegistryEntry *options = plane ? IORegistryEntry::fromPath("/options", plane) : nullptr;
    if (!options) return false;
    uint32_t value=0;
    OSData *off=OSData::withBytes(&value,sizeof(value));
    bool ok=off && options->setProperty("xegfx-disp-once",off);
    if (off) off->release();
    value=1;
    ok=ok && readGate(options,"xegfx-disp-once",&value) && value==0;
    options->release();
    return ok;
}
static bool
xg_trial_once_begin(void)
{
	IORegistryEntry *o = IORegistryEntry::fromPath("/options", gIODTPlane);
	if (o == nullptr)
		return false;
	OSObject *value = o->copyProperty("xegfx-disp-once");
	if (value == nullptr) {
		o->release();
		return true; /* Existing upstream installations have no one-shot key. */
	}
	OSData *d = OSDynamicCast(OSData, value);
	uint32_t armed = 0;
	bool validOnce = d != nullptr && d->getLength() == sizeof(armed);
	if (validOnce)
		memcpy(&armed, d->getBytesNoCopy(), sizeof(armed));
	value->release();
	bool ok = validOnce && armed == 0 &&
	    xg_resource_flag("WTLArrowEarlyClaimActive") && xg_resource_flag("WTLArrowTrialActive");
	if (armed == 1) {
		uint32_t zero = 0;
		OSData *off = OSData::withBytes(&zero, sizeof(zero));
		if (off != nullptr) {
			ok = o->setProperty("xegfx-disp-once", off);
			off->release();
		}
		OSObject *check = o->copyProperty("xegfx-disp-once");
		OSData *saved = OSDynamicCast(OSData, check);
		uint32_t got = 1;
		if (saved != nullptr && saved->getLength() == sizeof(got))
			memcpy(&got, saved->getBytesNoCopy(), sizeof(got));
		ok = ok && got == 0;
		OSSafeReleaseNULL(check);
	}
	o->release();
	if (ok)
		IOService::publishResource("WTLArrowTrialActive", kOSBooleanTrue);
	IOLog("XeGfx: one-shot trial %s; next boot does not auto-arm this trial\n", ok ? "consumed" : "not armed");
	return ok;
}

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
