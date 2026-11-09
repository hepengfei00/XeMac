#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>

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
class WTLArrowEarlyClaim : public IOService {
    OSDeclareDefaultStructors(WTLArrowEarlyClaim)
public:
    IOService *probe(IOService *provider,SInt32 *score) override {
        if (!armed()) return nullptr;
        if (score && *score<20150) *score=20150;
        return this;
    }
    bool start(IOService *provider) override {
        if (!armed() || !IOService::start(provider)) return false;
        if (!consume()) return false;
        setProperty("WTLArrowEarlyClaimActive",kOSBooleanTrue);
        setProperty("WTLArrowEarlyClaimVersion","1.2");
        IOService::publishResource("WTLArrowEarlyClaimActive",kOSBooleanTrue);
        IOService::publishResource("WTLArrowTrialActive",kOSBooleanTrue);
        IOService::publishResource("XeGfxBootArm",kOSBooleanTrue);
        IOLog("WTLArrowEarlyClaim: reserved Intel DP framebuffer category; one-shot consumed; GPU stages remain for the explicit loader\n");
        return true;
    }
};
OSDefineMetaClassAndStructors(WTLArrowEarlyClaim,IOService)
extern "C" kern_return_t _start(kmod_info_t *,void *);
extern "C" kern_return_t _stop(kmod_info_t *,void *);
extern "C" { KMOD_EXPLICIT_DECL(org.local.WTLArrowEarlyClaim, "1.2", _start, _stop) }
extern "C" kern_return_t _start(kmod_info_t *,void *) {return KERN_SUCCESS;}
extern "C" kern_return_t _stop(kmod_info_t *,void *) {return KERN_FAILURE;}
