/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#include "nvrm_off.h"
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <sys/sysctl.h>
#include <kern/thread_call.h>
#include <kern/clock.h>          // clock_get_uptime/nanoseconds_to_absolutetime: the 0x980 rescan throttle
#include <libkern/OSAtomic.h>
#include <libkern/c++/OSBoolean.h>   // kOSBooleanTrue: pointer-equality read of the accelerator's request property
#define FBLOG(fmt, ...) IOLog("NVRM-agdc: " fmt "\n", ##__VA_ARGS__)
#include <mach/kmod.h>
extern "C" kern_return_t _start(kmod_info_t *ki, void *d); extern "C" kern_return_t _stop(kmod_info_t *ki, void *d);
#ifdef NMX_XEGFX
// the same AGDC for the Intel GPU over XeGfx (branch xe): IOPresentment pairs a framebuffer with its display pipe only
// through an AGDC service on the GPU, so without one WindowServer finds no pipe for the boot framebuffer (N-249).
// Own class, bundle and sysctl names, because NVRMAGDC loads on the same machine.
extern "C" { KMOD_EXPLICIT_DECL(org.watchthelight.XeGfxAGDC, "0.1", _start, _stop) }
#define NVDADeviceControl XeGfxDeviceControl
#define NVDAGDCLoader     XeGfxAGDCLoader
#define AGDC_FB_CLASS     "IONDRVFramebuffer"
// The end-state KMS adapter is a direct IOFramebuffer subclass, so serviceMatching("IONDRVFramebuffer") never sees it
// (that matches the class and its subclasses only).  Both names are enumerated; the legacy one stays first so today's
// selection order is unchanged while both kexts are loaded (see nvdaAgdcTargets/nvdaAgdcScan).
#define AGDC_FB_CLASS_ALT "XeGfxFramebuffer"
#define AGDC_VENDOR       "Intel"
#define AGDC_VENDOR_ID    0x8086
#define nvrmfb_agdc xegfx_agdc
#define sysctl__debug_nvrmfb_agdc sysctl__debug_xegfx_agdc
#define nvrmfb_agdc_cmds xegfx_agdc_cmds
#define sysctl__debug_nvrmfb_agdc_cmds sysctl__debug_xegfx_agdc_cmds
#define nvrmfb_agdc_refused xegfx_agdc_refused
#define sysctl__debug_nvrmfb_agdc_refused sysctl__debug_xegfx_agdc_refused
#define nvrmfb_agdc_last_refused xegfx_agdc_last_refused
#define sysctl__debug_nvrmfb_agdc_last_refused sysctl__debug_xegfx_agdc_last_refused
#define nvrmfb_agdc_k5 xegfx_agdc_k5
#define sysctl__debug_nvrmfb_agdc_k5 sysctl__debug_xegfx_agdc_k5
#define nvrmfb_agdc_k5_link xegfx_agdc_k5_link
#define sysctl__debug_nvrmfb_agdc_k5_link sysctl__debug_xegfx_agdc_k5_link
#define nvrmfb_agdc_k5_caps xegfx_agdc_k5_caps
#define sysctl__debug_nvrmfb_agdc_k5_caps sysctl__debug_xegfx_agdc_k5_caps
#define nvrmfb_agdc_k5_why xegfx_agdc_k5_why
#define sysctl__debug_nvrmfb_agdc_k5_why sysctl__debug_xegfx_agdc_k5_why
#define nvrmfb_agdc_fbmap xegfx_agdc_fbmap
#define sysctl__debug_nvrmfb_agdc_fbmap sysctl__debug_xegfx_agdc_fbmap
#define nvrmfb_agdc_rescans xegfx_agdc_rescans
#define sysctl__debug_nvrmfb_agdc_rescans sysctl__debug_xegfx_agdc_rescans
#define nvrmfb_agdc_maxfb xegfx_agdc_maxfb
#define sysctl__debug_nvrmfb_agdc_maxfb sysctl__debug_xegfx_agdc_maxfb
#define nvrmfb_agdc_k5_asked xegfx_agdc_k5_asked
#define sysctl__debug_nvrmfb_agdc_k5_asked sysctl__debug_xegfx_agdc_k5_asked
#define nvrmfb_agdc_k5_link_mask xegfx_agdc_k5_link_mask
#define sysctl__debug_nvrmfb_agdc_k5_link_mask sysctl__debug_xegfx_agdc_k5_link_mask
#define nvrmfb_agdc_k5_caps_mask xegfx_agdc_k5_caps_mask
#define sysctl__debug_nvrmfb_agdc_k5_caps_mask sysctl__debug_xegfx_agdc_k5_caps_mask
#define nvrmfb_agdc_k5_last_refused_idx xegfx_agdc_k5_last_refused_idx
#define sysctl__debug_nvrmfb_agdc_k5_last_refused_idx sysctl__debug_xegfx_agdc_k5_last_refused_idx
#define nvrmfb_agdc_client_ack xegfx_agdc_client_ack
#define sysctl__debug_nvrmfb_agdc_client_ack sysctl__debug_xegfx_agdc_client_ack
#define nvrmfb_agdc_client_set xegfx_agdc_client_set
#define sysctl__debug_nvrmfb_agdc_client_set sysctl__debug_xegfx_agdc_client_set
#else
extern "C" { KMOD_EXPLICIT_DECL(com.nullmoth.NVRMAGDC, "0.1", _start, _stop) }
#define AGDC_FB_CLASS     "NVRMNVDAFramebuffer"
#define AGDC_VENDOR       "NVIDIA"
#define AGDC_VENDOR_ID    0x10de
#endif
// Framebuffer classes this AGDC serves, in match order.  Under NMX_XEGFX the legacy IONDRVFramebuffer subclass stays
// first: while the placeholder XeGfxIntelFB and the KMS adapter are both loaded, the placeholder keeps the head.
#ifdef NMX_XEGFX
static const char *const gAgdcFBClass[] = { AGDC_FB_CLASS, AGDC_FB_CLASS_ALT };
#define AGDC_FB_CLASS_COUNT (sizeof gAgdcFBClass / sizeof gAgdcFBClass[0])
#else
static const char *const gAgdcFBClass[] = { AGDC_FB_CLASS };
#define AGDC_FB_CLASS_COUNT (sizeof gAgdcFBClass / sizeof gAgdcFBClass[0])
#endif
extern "C" kern_return_t _start(kmod_info_t *ki, void *d) { return KERN_SUCCESS; }
extern "C" kern_return_t _stop(kmod_info_t *ki, void *d)  { return KERN_SUCCESS; }
#include "AppleGraphicsDeviceControl_re.h"
#include "nvrm-agdc-k5.h"
#pragma pack(push, 1)
struct NVAGDCVendorInfo {
    uint32_t version;
    char     vendor[0x20];
    uint32_t vendorID;
    uint32_t vendorClass;
};
struct NVAGDCGPUCapability {
    uint64_t portMask;
    uint64_t portFeatureA;
    uint64_t portFeatureB;
    uint64_t portFeatureC;
    uint32_t countA, countB, countC, countD;
    uint32_t maxFramebuffers;
    IOService *pciDevice;
    IOService *framebuffer[20];
};
#pragma pack(pop)
static_assert(sizeof(NVAGDCVendorInfo) == 0x2c, "AGDCVendorInfo_t is 0x2c");
static_assert(sizeof(NVAGDCGPUCapability) == 0xdc, "AGDCGPUCapability_t is 0xdc");
static void nvdaAgdcPublish(void);   // the read-only observables; called from the 0x980 path, the worker and the poll

static int    gAgdcK5 = 0;
static SInt64 gAgdcK5Colour = 0;   // §37.12: colour-stage capability queries answered
static SInt64 gAgdcK5Link = 0, gAgdcK5Caps = 0;
static int    gAgdcK5Why = 0;
static SInt64 gAgdcCmds = 0, gAgdcUnsupported = 0;
static int    gAgdcLastRefused = 0;
static int    gAgdcLogged = 0;
// WindowServer's 4-byte "client set" (0x2001). thanhnndev's AGDC trace (nvidia-macos-driver e196ab19) saw CoreDisplay
// submit display-pipe transactions only once it was accepted; Intel-only until an NVIDIA boot shows the same.
#ifdef NMX_XEGFX
static int    gAgdcClientAck = 1;
#else
static int    gAgdcClientAck = 0;
#endif
static SInt64 gAgdcClientSet = 0;
// Slot-table rescans done while answering 0x980 (see nvdaAgdcRescanForReply).
static SInt64 gAgdcRescans = 0;

enum { kAgdcMaxFB = 4 };
static IOService *gAgdcFB[kAgdcMaxFB];
static unsigned  gAgdcNFB = 0;
enum { kAgdcFBMapBytes = 3 + kAgdcMaxFB * 25 + 52 + 1 };
static_assert(kAgdcMaxFB >= 1 && kAgdcMaxFB <= 8, "IOPresentment's map holds 8 (kext 0x31b3), and kAgdcFBMapBytes assumes one digit for n and each slot index");
static char   gAgdcFBMap[kAgdcFBMapBytes] = "never scanned";
static int    gAgdcMaxFBOut = 0;
static int    gAgdcK5Asked = 0, gAgdcK5LinkMask = 0, gAgdcK5CapsMask = 0;
static int    gAgdcK5LastRefusedIdx = -1;

class NVDADeviceControl : public AppleGraphicsDeviceControl {
    OSDeclareDefaultStructors(NVDADeviceControl)
public:
    IOService *fPci; IOService *fFB;
    using AppleGraphicsDeviceControl::vendor_doDeviceAttribute;
    virtual IOReturn vendor_doDeviceAttribute(unsigned int cmd, unsigned long *in, unsigned long inSize,
                                              unsigned long *out, unsigned long *outSize, IOExternalMethodArguments *args) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(NVDADeviceControl, AppleGraphicsDeviceControl)

static IOReturn nvdaK5Answer(IOService *fb, unsigned int cmd, unsigned long *in, unsigned long inSize, unsigned long *out, unsigned long *outSize)
{
    IOService *slot[kAgdcMaxFB]; unsigned nh = gAgdcNFB;
    if (nh == 0 || nh > kAgdcMaxFB) { slot[0] = fb; nh = 1; } else for (unsigned i = 0; i < nh; i++) slot[i] = gAgdcFB[i];
    uint32_t t[kAgdcMaxFB][NVK5_TIMING_WORDS]; struct nvk5_head h[kAgdcMaxFB];
    for (unsigned i = 0; i < nh; i++) {
        unsigned long tBytes = 0; bool have = false;
        OSObject *o = slot[i] ? slot[i]->copyProperty("NVRMTiming") : NULL; OSData *d = OSDynamicCast(OSData, o);
        if (d) { have = true; tBytes = d->getLength(); bzero(t[i], sizeof t[i]); if (tBytes == sizeof t[i]) memcpy(t[i], d->getBytesNoCopy(), sizeof t[i]); }
        OSSafeReleaseNULL(o);
        // No fabricated panel timing: wait until the framebuffer publishes NVRMTiming.

        h[i].t = have ? t[i] : NULL; h[i].tBytes = tBytes;
    }
    int why = 0; int32_t idx = -1;
    const uint32_t capType = (cmd == NVK5_CMD_FBCAPEX && in && inSize >= 12) ? ((const uint32_t *)in)[1] : 0;
    const uint32_t capCount = (cmd == NVK5_CMD_FBCAPEX && in && inSize >= 12) ? ((const uint32_t *)in)[2] : 0;
    int r = nvk5_answer_heads(cmd, in, inSize, out, outSize ? *outSize : 0, h, nh, &why, &idx);
    // §37.13: the plane entry as Apple's ICL framebuffer builds it (getDisplayPipeCapabilityExtended, type 0x10): both
    // size arrays hold the current desktop (min = max = the framebuffer's source size, not the panel's native timing),
    // each entry's format group is BGRA (0x10000) and its sixth word is 2.  With the panel's 2560x1600 there, a 3840x2400
    // HiDPI composite failed CoreDisplay's sub-region / scaling checks and went through GPUPass every frame.
    if (cmd == NVK5_CMD_FBCAPEX && capType == NVK5_CAP_PLANESCALER && r == NVK5_R_CAPS && nvk5_bgra_on && out
        && idx >= 0 && (unsigned)idx < nh && slot[idx]) {
        OSNumber *dn = OSDynamicCast(OSNumber, slot[idx]->getProperty("XeGfxDesktop"));
        const uint64_t dv = dn ? dn->unsigned64BitValue() : 0;
        const uint32_t dw = (uint32_t)(dv >> 32), dh = (uint32_t)dv;
        if (dw >= 640 && dh >= 400 && dw <= 16384 && dh <= 16384) {
            uint8_t *e = ((struct NVAGDCFBExtCap *)out)->entry[0];
            for (unsigned a = 0x24; a <= 0x1ac; a += 0x188) {
                nvk5_put32(e, a + 0x00, dh); nvk5_put32(e, a + 0x04, dw); nvk5_put32(e, a + 0x08, dh); nvk5_put32(e, a + 0x0c, dw);
                nvk5_put32(e, a + 0x10, 0x10000); nvk5_put32(e, a + 0x14, 2);
            }
            static SInt32 told; if (OSIncrementAtomic(&told) < 4) FBLOG("AGDC 0x711: plane 0 limits set to the desktop %ux%u (Apple layout)", dw, dh);
        }
    }
    // §37.12: every extended-capability query, the first 64: which colour stages CoreDisplay asks about
    if (cmd == NVK5_CMD_FBCAPEX) {
        static SInt32 said;
        if (capType == 1 || capType == 2 || capType == 4) { if (r == NVK5_R_CAPS) OSAddAtomic64(1, &gAgdcK5Colour); }
        if (OSIncrementAtomic(&said) < 64)
            FBLOG("AGDC 0x711: fb %d type %#x count %u -> %s", idx, capType, capCount,
                  r == NVK5_R_CAPS ? (capType == NVK5_CAP_PLANESCALER ? "plane scaler" : "colour stage answered") : r == NVK5_R_UNSUPPORTED ? "Unsupported" : "refused");
    }
    const UInt32 bit = (idx >= 0 && idx < 32) ? (1u << idx) : 0;
    if (bit) OSBitOrAtomic(bit, (volatile UInt32 *)&gAgdcK5Asked);
    if (r == NVK5_R_LINK) { OSAddAtomic64(1, &gAgdcK5Link); OSBitOrAtomic(bit, (volatile UInt32 *)&gAgdcK5LinkMask); return kIOReturnSuccess; }
    if (r == NVK5_R_CAPS) { OSAddAtomic64(1, &gAgdcK5Caps); OSBitOrAtomic(bit, (volatile UInt32 *)&gAgdcK5CapsMask); return kIOReturnSuccess; }
    if (r == NVK5_R_UNSUPPORTED) return kIOReturnUnsupported;
    if (r == NVK5_R_NOTFOUND && idx >= 0 && (unsigned)idx < nh && !slot[idx]) why = 0x30;
    gAgdcK5Why = why; gAgdcK5LastRefusedIdx = idx;
    if (gAgdcLogged < 40) { gAgdcLogged++; FBLOG("AGDC K5: command %#x index %d NOT answered (why %#x, %u slot(s)) [%d/40]", cmd, idx, why, nh, gAgdcLogged); }
    return r == NVK5_R_BADARG ? kIOReturnBadArgument : kIOReturnNotFound;
}

static void nvdaAgdcScan(IOService *pci);
#ifdef NMX_XEGFX
// Applies the K5 half of a boot-armed XeGfxAccelAgdcRequest that showed up after userspace had already armed AGDC.
static void nvdaAgdcNoteRequest(IOService *pci);
#endif
// WindowServer reads AGDC command 0x980 when it (re)starts and builds one display pipe per entry in the reply. The
// slot table used to be scanned only once, when AGDC came up, so a framebuffer that started later -- a monitor
// plugged in after boot -- was never in the answer: WindowServer then built a single display pipe, the second
// display's composited frames had no pipe to go to, and the panel showed only its cursor while `screencapture -D 2`
// showed a full desktop (iop_ok_h0 grew, iop_ok_h1 stayed 0). Re-scan before answering, at most once a second and
// logging only when the count changes, so the reply is never stale. This runs in the vendor-call context, which
// holds no registry lock, and nvdaAgdcScan only matches services and reads their properties.
static void nvdaAgdcRescanForReply(IOService *pci)
{
    if (!pci) return;
#ifdef NMX_XEGFX
    // The accelerator sets its request only at 6/6, after the framebuffer pipeline is up; this is where the K5 half
    // of a boot-armed request reaches an AGDC that userspace had already brought up (the old two-sysctl flow).
    nvdaAgdcNoteRequest(pci);
#endif
    static uint64_t last = 0;
    uint64_t now = 0; clock_get_uptime(&now);
    uint64_t per = 0; nanoseconds_to_absolutetime(1000000000ull, &per);
    if (last && now - last < per) return;
    last = now;
    const unsigned before = gAgdcNFB;
    nvdaAgdcScan(pci);
    OSAddAtomic64(1, &gAgdcRescans);
    if (gAgdcNFB != before)
        FBLOG("AGDC: 0x980 rescan -> %u framebuffer slot(s) (was %u)", gAgdcNFB, before);
    nvdaAgdcPublish();
}

IOReturn NVDADeviceControl::vendor_doDeviceAttribute(unsigned int cmd, unsigned long *in, unsigned long inSize,
                                                     unsigned long *out, unsigned long *outSize, IOExternalMethodArguments *args)
{
    OSAddAtomic64(1, &gAgdcCmds);
    if (cmd == 1) {
        if (!out || !outSize || *outSize != sizeof(NVAGDCVendorInfo)) return kIOReturnBadArgument;
        NVAGDCVendorInfo *v = (NVAGDCVendorInfo *)out; bzero(v, sizeof *v);
        v->version = 0x30000; strlcpy(v->vendor, AGDC_VENDOR, sizeof v->vendor); v->vendorID = AGDC_VENDOR_ID; v->vendorClass = 2;
        return kIOReturnSuccess;
    }
    if (cmd == 0x980) {
        if (!out || !outSize || *outSize != sizeof(NVAGDCGPUCapability)) return kIOReturnBadArgument;
        nvdaAgdcRescanForReply(fPci);   // a framebuffer that started after boot has to be in this reply
        NVAGDCGPUCapability *c = (NVAGDCGPUCapability *)out; bzero(c, sizeof *c);
        unsigned n = gAgdcNFB;
        if (n == 0 || n > kAgdcMaxFB) {
            c->portMask = 1ull << 1;
            c->countA = c->countB = c->countC = c->countD = 1;
            c->maxFramebuffers = 1; c->pciDevice = fPci; c->framebuffer[0] = fFB;
        } else {
            for (unsigned i = 0; i < n; i++) { c->framebuffer[i] = gAgdcFB[i]; c->portMask |= 1ull << (1 + i); }
            c->countA = c->countB = c->countC = c->countD = n;
            c->maxFramebuffers = n; c->pciDevice = fPci;
        }
        gAgdcMaxFBOut = (int)c->maxFramebuffers;
        return kIOReturnSuccess;
    }
    if (cmd == 0x2001 && gAgdcClientAck) {
        if (!in || inSize != 4) return kIOReturnBadArgument;
        OSAddAtomic64(1, &gAgdcClientSet);
        return kIOReturnSuccess;
    }
    if (gAgdcK5 && (cmd == NVK5_CMD_LINKCONFIG || cmd == NVK5_CMD_FBCAPEX)) {
        IOReturn k5 = nvdaK5Answer(fFB, cmd, in, inSize, out, outSize);
        if (k5 != kIOReturnUnsupported) return k5;
    }
    OSAddAtomic64(1, &gAgdcUnsupported); gAgdcLastRefused = (int)cmd;
    if (gAgdcLogged < 40) { gAgdcLogged++; FBLOG("AGDC: command %#x (in %lu bytes) -> Unsupported [%d/40]", cmd, inSize, gAgdcLogged); }
    return kIOReturnUnsupported;
}

static int gAgdcState = 0; static NVDADeviceControl *gAgdc = nullptr; static thread_call_t gAgdcCall = nullptr; static int gAgdcWant = 0;
static IOService *gAgdcFBRef = nullptr;
static SInt32 gAgdcBusy = 0;   // moved up from the worker: the auto poll checks it before retrying an arm

// ---------------------------------------------------------------------------------------------------------------
// M3d: in-kernel replacement for the two userspace actions that used to finish the AGDC takeover
// (`sysctl debug.xegfx_agdc=1` and `debug.xegfx_agdc_k5=1`), plus the read-only observables bootguard reads.
//
// The boot-armed accelerator thread cannot write another kext's sysctls (docs/19-M4A-ACCEL.md §8.2), so at 6/6 it
// leaves XeGfxAccelAgdcRequest=true on the Intel GPU's IOPCIDevice node (nvrm-accel.cpp bootArmRun).  This loader's
// provider is IOResources (Info.plist), so the request is read by matching the PCI node, not via provider; once the
// AppleGraphicsDeviceControl exists its provider *is* that node and the 0x980 path reads it there again.  Everything
// here is read-only against the registry and never touches NVRAM, a sysctl, or a display register.
// ---------------------------------------------------------------------------------------------------------------
#ifdef NMX_XEGFX
static IOService *gAgdcLoaderSvc = nullptr;       // the loader service: mailbox for the observables
static const char *gAgdcTargetClass = nullptr;    // gAgdcFBClass[ci] of the framebuffer in gAgdcFBRef
static char  gAgdcFailText[64] = "none";          // last worker failure, for xegfx-agdc-fail
static int   gAgdcRequestSeen = 0;                // XeGfxAccelAgdcRequest was observed at least once
// Automatic arming state machine: idle, waiting for the request, armed (applied; the poll now only watches the
// worker come up and retries the no-framebuffer-yet failure), timeout (userspace may still do the old writes),
// manual (userspace wrote agdc=1 itself; the poll is cancelled).
enum { kAgdcAutoIdle, kAgdcAutoWaitReq, kAgdcAutoArmed, kAgdcAutoTimeout, kAgdcAutoManual, kAgdcAutoManualRetry };
static int    gAgdcAutoState = kAgdcAutoIdle;
static int    gAgdcAutoEnabled = 0;
static thread_call_t gAgdcAutoCall = nullptr;
static SInt32 gAgdcAutoBusy = 0;
static SInt64 gAgdcAutoPolls = 0;
static uint64_t gAgdcAutoDeadline = 0, gAgdcAutoLastArm = 0;
static char   gAgdcAutoWhy[96] = "automatic arming not started";
static int    gAgdcAltLogged = 0;
static void nvdaAgdcWork(thread_call_param_t, thread_call_param_t);

// A property read from a thread-call context: copyProperty takes the registry lock itself and we hold none.
static bool nvdaAgdcPropTrue(IORegistryEntry *e, const char *key)
{
    if (!e) return false;
    OSObject *o = e->copyProperty(key);
    bool on = (o == kOSBooleanTrue);   // the request is stored as kOSBooleanTrue; pointer equality is the API
    if (!on) { OSNumber *n = OSDynamicCast(OSNumber, o); if (n) on = n->unsigned64BitValue() != 0; }
    OSSafeReleaseNULL(o);
    return on;
}
static uint32_t nvdaAgdcPropU32(IORegistryEntry *e, const char *key, bool *have)
{
    uint32_t v = 0; if (have) *have = false;
    if (!e) return 0;
    OSObject *o = e->copyProperty(key); OSNumber *n = OSDynamicCast(OSNumber, o);
    if (n) { v = n->unsigned32BitValue(); if (have) *have = true; }
    else { OSData *d = OSDynamicCast(OSData, o); if (d && d->getLength() >= 4) { v = *(const uint32_t *)d->getBytesNoCopy(); if (have) *have = true; } }
    OSSafeReleaseNULL(o);
    return v;
}

// The end-state adapter (AGDC_FB_CLASS_ALT) is a legitimate head only once it really is the live framebuffer.  In a
// takeover boot the adapter publishes phase==2/trial==1, read from NVRAM, on its own node and is the only framebuffer
// that exists; outside those gates a node must be registered *and* past a readiness gate.  The inert second
// framebuffer that phase 1 publishes carries XeGfxFbRegistered=true but neither XeGfxL0Ready nor XeGfxFbModeReady, so
// it never enters the slot table: the legacy IONDRVFramebuffer keeps the head exactly as before.
static bool nvdaAgdcAltEligible(IOService *f, const char **whyOut)
{
    const char *why = "no node";
    bool ok = false;
    if (f) {
        bool havePhase = false, haveTrial = false;
        uint32_t phase = nvdaAgdcPropU32(f, "xegfx-disp-phase", &havePhase);
        uint32_t trial = nvdaAgdcPropU32(f, "xegfx-disp-trial", &haveTrial);
        if (havePhase && haveTrial && phase == 2 && trial == 1) { ok = true; why = "takeover gates phase=2 trial=1"; }
        else if (!nvdaAgdcPropTrue(f, "XeGfxFbRegistered")) { why = "not registered yet"; }
        else if (!nvdaAgdcPropTrue(f, "XeGfxL0Ready") && !nvdaAgdcPropTrue(f, "XeGfxFbModeReady")) { why = "registered without a readiness gate (the inert second framebuffer)"; }
        else { ok = true; why = "registered with a readiness gate"; }
    }
    if (whyOut) *whyOut = why;
    return ok;
}
static const char *nvdaAgdcStateText(void)
{
    switch (gAgdcState) {
    case 0:  return "not-up";
    case 1:  return "up";
    case 2:  return "terminated";
    default: return "failed";
    }
}
static void nvdaAgdcNoteRequest(IOService *pci)
{
    if (!pci || !nvdaAgdcPropTrue(pci, "XeGfxAccelAgdcRequest")) return;
    gAgdcRequestSeen = 1;
    if (gAgdcK5 == 0) { gAgdcK5 = 1; FBLOG("AGDC auto: XeGfxAccelAgdcRequest on %s -> K5 enabled (AGDC was already up)", pci->getName()); }
    nvdaAgdcPublish();
}
#endif

// Read-only diagnostics for bootguard / post-mortem, published on the loader service (`ioreg -c XeGfxAGDCLoader`).
// Nothing reads them back and they never change behavior.
static void nvdaAgdcPublish(void)
{
#ifdef NMX_XEGFX
    IOService *s = gAgdcLoaderSvc; if (!s) return;
    const char *autoTxt = "idle";
    switch (gAgdcAutoState) {
    case kAgdcAutoWaitReq: autoTxt = "waiting for XeGfxAccelAgdcRequest"; break;
    case kAgdcAutoArmed:   autoTxt = "armed: XeGfxAccelAgdcRequest applied in-kernel"; break;
    case kAgdcAutoTimeout: autoTxt = "timeout: no XeGfxAccelAgdcRequest"; break;
    case kAgdcAutoManual:  autoTxt = "manual: debug.xegfx_agdc=1 by userspace"; break;
    case kAgdcAutoManualRetry: autoTxt = "manual: debug.xegfx_agdc=1 by userspace, retrying until a framebuffer is eligible"; break;
    default: break;
    }
    s->setProperty("xegfx-agdc-state", nvdaAgdcStateText());
    s->setProperty("xegfx-agdc-fail", gAgdcFailText);
    s->setProperty("xegfx-agdc-auto", autoTxt);
    s->setProperty("xegfx-agdc-auto-why", gAgdcAutoWhy);
    s->setProperty("xegfx-agdc-request", (unsigned long long)(gAgdcRequestSeen ? 1 : 0), 32);
    s->setProperty("xegfx-agdc-polls", (unsigned long long)(uint64_t)gAgdcAutoPolls, 32);
    s->setProperty("xegfx-agdc-fb-class", gAgdcTargetClass ? gAgdcTargetClass : "none");
    s->setProperty("xegfx-agdc-fb-map", gAgdcFBMap);
    s->setProperty("xegfx-agdc-maxfb", (unsigned long long)(unsigned)gAgdcMaxFBOut, 32);
    s->setProperty("xegfx-agdc-k5", (unsigned long long)(gAgdcK5 ? 1 : 0), 32);
    s->setProperty("xegfx-agdc-k5-why", (unsigned long long)(unsigned)gAgdcK5Why, 32);
    s->setProperty("xegfx-agdc-rescans", (unsigned long long)(uint64_t)gAgdcRescans, 64);
#endif
}
// One place for the worker's failure/success bookkeeping: the state code, the fail text and the observables.
static void nvdaAgdcFailNote(int code, const char *text)
{
    gAgdcState = code;
#ifdef NMX_XEGFX
    snprintf(gAgdcFailText, sizeof gAgdcFailText, "%s", text ? text : (code < 0 ? "failed" : "none"));
    nvdaAgdcPublish();
#else
    (void)text;
#endif
}

#ifdef NMX_XEGFX
// Find the Intel GPU node that carries the accelerator's request; returns a retained service or nullptr.
static IOService *nvdaAgdcFindRequestPci(void)
{
    OSDictionary *m = IOService::serviceMatching("IOPCIDevice");
    OSIterator *it = m ? IOService::getMatchingServices(m) : nullptr; OSSafeReleaseNULL(m);
    IOService *hit = nullptr;
    if (it) {
        OSObject *o;
        while (!hit && (o = it->getNextObject())) {
            IOService *p = OSDynamicCast(IOService, o); if (!p) continue;
            OSData *v = OSDynamicCast(OSData, p->getProperty("vendor-id"));
            if (!v || v->getLength() < 2 || *(const uint16_t *)v->getBytesNoCopy() != AGDC_VENDOR_ID) continue;
            if (!nvdaAgdcPropTrue(p, "XeGfxAccelAgdcRequest")) continue;
            hit = p; hit->retain();
        }
        it->release();
    }
    return hit;
}
// Apply exactly what the two sysctl writes did: gAgdcK5=1, then gAgdcWant=1 + a worker thread call.
static bool nvdaAgdcAutoApply(IOService *pci)
{
    uint64_t now = 0; clock_get_uptime(&now);
    FBLOG("AGDC auto: XeGfxAccelAgdcRequest on %s -> applying debug.xegfx_agdc_k5=1 and debug.xegfx_agdc=1 in-kernel (no userspace write)",
          pci ? pci->getName() : "?");
    gAgdcK5 = 1;
    gAgdcWant = 1;
    if (!gAgdcCall) gAgdcCall = thread_call_allocate(nvdaAgdcWork, nullptr);
    if (!gAgdcCall) {
        gAgdcAutoEnabled = 0; gAgdcAutoState = kAgdcAutoTimeout;
        snprintf(gAgdcAutoWhy, sizeof gAgdcAutoWhy, "thread call allocation failed: userspace must write debug.xegfx_agdc=1/debug.xegfx_agdc_k5=1");
        FBLOG("AGDC auto: %s", gAgdcAutoWhy);
        nvdaAgdcFailNote(-6, "auto-arm-thread-call-failed");
        return false;
    }
    gAgdcAutoLastArm = now;
    thread_call_enter(gAgdcCall);
    gAgdcAutoState = kAgdcAutoArmed;
    snprintf(gAgdcAutoWhy, sizeof gAgdcAutoWhy, "armed by XeGfxAccelAgdcRequest on %s (k5=1, want=1)", pci ? pci->getName() : "?");
    nvdaAgdcPublish();
    return true;
}
// Poll context (thread call): bounded registry work, never on the IOWorkLoop, never blocking a sysctl or vendor call.
static void nvdaAgdcAutoPoll(thread_call_param_t, thread_call_param_t)
{
    OSIncrementAtomic(&gAgdcAutoBusy);
    if (gAgdcAutoEnabled) {
        uint64_t now = 0; clock_get_uptime(&now);
        uint64_t per = 0; nanoseconds_to_absolutetime(500000000ull, &per);
        OSAddAtomic64(1, &gAgdcAutoPolls);
        IOService *pci = nvdaAgdcFindRequestPci();
        if (pci) {
            gAgdcRequestSeen = 1;
            if (gAgdcAutoState == kAgdcAutoWaitReq) nvdaAgdcAutoApply(pci);
            pci->release();
        }
        // a userspace write (kAgdcAutoManualRetry) is supervised exactly like the in-kernel arm
        bool armed = gAgdcAutoState == kAgdcAutoArmed || gAgdcAutoState == kAgdcAutoManualRetry;
        if (armed && gAgdcState == 1) {
            gAgdcAutoEnabled = 0;
            snprintf(gAgdcAutoWhy, sizeof gAgdcAutoWhy, "done: AGDC up on %u framebuffer slot(s), K5 on", gAgdcNFB);
            FBLOG("AGDC auto: done — AGDC is UP (%u framebuffer slot(s)); the userspace two-sysctl step is no longer needed", gAgdcNFB);
            nvdaAgdcPublish();
        } else if (gAgdcAutoEnabled && (now >= gAgdcAutoDeadline || gAgdcState == 2)) {
            gAgdcAutoEnabled = 0; gAgdcAutoState = kAgdcAutoTimeout;
            snprintf(gAgdcAutoWhy, sizeof gAgdcAutoWhy, "gave up at the 900 s budget (state %s, %lld poll(s)); userspace can still write debug.xegfx_agdc=1/debug.xegfx_agdc_k5=1",
                     nvdaAgdcStateText(), (long long)gAgdcAutoPolls);
            FBLOG("AGDC auto: %s", gAgdcAutoWhy);
            nvdaAgdcPublish();
        } else if (armed && gAgdcState <= -2) {
            // a hard bring-up failure will not fix itself by polling: report it once and stop the poll
            gAgdcAutoEnabled = 0; gAgdcAutoState = kAgdcAutoTimeout;
            snprintf(gAgdcAutoWhy, sizeof gAgdcAutoWhy, "bring-up failed: %s; no retry (userspace can still write debug.xegfx_agdc=1/debug.xegfx_agdc_k5=1)", gAgdcFailText);
            FBLOG("AGDC auto: %s", gAgdcAutoWhy);
            nvdaAgdcPublish();
        } else if (armed && gAgdcCall && (gAgdcState == 0 || gAgdcState == -1) && !gAgdcBusy && now - gAgdcAutoLastArm >= per) {
            // no framebuffer candidate yet (the request can beat the adapter's registration): retry the worker
            gAgdcAutoLastArm = now;
            thread_call_enter(gAgdcCall);
        }
        if (gAgdcAutoEnabled) thread_call_enter_delayed(gAgdcAutoCall, now + per);
    }
    OSDecrementAtomic(&gAgdcAutoBusy);
}
// Called from the loader's start(): one immediate look, then a 500 ms poll bounded by a 900 s budget.
static bool nvdaAgdcAutoStart(IOService *loader)
{
    gAgdcLoaderSvc = loader;
    gAgdcAutoState = kAgdcAutoWaitReq;
    gAgdcAutoEnabled = 1;
    uint64_t now = 0; clock_get_uptime(&now);
    uint64_t budget = 0; nanoseconds_to_absolutetime(900000000000ull, &budget);
    gAgdcAutoDeadline = now + budget;
    gAgdcAutoCall = thread_call_allocate(nvdaAgdcAutoPoll, nullptr);
    if (!gAgdcAutoCall) {
        gAgdcAutoEnabled = 0; gAgdcAutoState = kAgdcAutoIdle;
        snprintf(gAgdcAutoWhy, sizeof gAgdcAutoWhy, "thread call allocation failed: userspace must write debug.xegfx_agdc=1/debug.xegfx_agdc_k5=1");
        FBLOG("AGDC auto: %s", gAgdcAutoWhy);
        nvdaAgdcPublish();
        return false;
    }
    IOService *pci = nvdaAgdcFindRequestPci();
    if (pci) {
        gAgdcRequestSeen = 1;
        FBLOG("AGDC auto: XeGfxAccelAgdcRequest already present on %s at load time", pci->getName());
        nvdaAgdcAutoApply(pci);   // the poll below only supervises the worker from here on
        pci->release();
    } else {
        snprintf(gAgdcAutoWhy, sizeof gAgdcAutoWhy, "waiting for XeGfxAccelAgdcRequest on the Intel GPU node (poll 500 ms, 900 s budget)");
        FBLOG("AGDC auto: %s", gAgdcAutoWhy);
    }
    uint64_t per = 0; nanoseconds_to_absolutetime(500000000ull, &per);
    thread_call_enter_delayed(gAgdcAutoCall, now + per);
    nvdaAgdcPublish();
    return true;
}
// Loader stop: disarm first, then cancel and free only once no invocation can be in flight (same bounded-wait policy
// as the worker below; if it still runs we leak the call rather than free it under a running callback).
static void nvdaAgdcAutoStop(void)
{
    gAgdcAutoEnabled = 0;
    if (!gAgdcAutoCall) return;
    thread_call_cancel(gAgdcAutoCall);
    int calm = 0, spins = 0;
    while (calm < 5 && spins++ < 200) { if (gAgdcAutoBusy) calm = 0; else calm++; IOSleep(20); }
    if (calm < 5) FBLOG("stop: the AGDC auto poll is STILL running after 4 s — leaving its thread call allocated rather than freeing it under it");
    else { thread_call_free(gAgdcAutoCall); gAgdcAutoCall = nullptr; }
}
// Userspace wrote debug.xegfx_agdc itself.  The write can come before the adapter framebuffer exists (core writes it
// right after stage 7, the framebuffer is published on its own schedule), so the poll keeps supervising it: a
// "no framebuffer yet" worker result is retried every 500 ms within the same 900 s budget, a hard failure still stops.
static void nvdaAgdcAutoNoteManual(void)
{
    if (gAgdcAutoState == kAgdcAutoWaitReq || gAgdcAutoState == kAgdcAutoArmed) {
        uint64_t now = 0; clock_get_uptime(&now);
        gAgdcAutoState = kAgdcAutoManualRetry; gAgdcAutoLastArm = now;
        snprintf(gAgdcAutoWhy, sizeof gAgdcAutoWhy, "debug.xegfx_agdc=1 written by userspace; retrying until a framebuffer is eligible");
        FBLOG("AGDC auto: userspace wrote debug.xegfx_agdc=1 — the poll keeps retrying the worker until a framebuffer is eligible");
        nvdaAgdcPublish();
    }
}
#endif

static void nvdaAgdcDropTable(void)
{
    unsigned n = gAgdcNFB; gAgdcNFB = 0;
    __asm__ __volatile__("" ::: "memory");
    for (unsigned i = 0; i < kAgdcMaxFB; i++) { IOService *f = gAgdcFB[i]; gAgdcFB[i] = nullptr; if (f) f->release(); }
    if (n) snprintf(gAgdcFBMap, sizeof gAgdcFBMap, "released (the last table had %u slot(s))", n);
}
static void nvdaAgdcDropFB(void) { if (gAgdcFBRef) { gAgdcFBRef->release(); gAgdcFBRef = nullptr; }
#ifdef NMX_XEGFX
    gAgdcTargetClass = nullptr;
#endif
    nvdaAgdcDropTable(); }
static void nvdaAgdcTargets(IOService **pci, IOService **fb)
{
    *pci = nullptr; *fb = nullptr;
    for (unsigned ci = 0; ci < AGDC_FB_CLASS_COUNT && !*fb; ci++) {
        OSDictionary *m = IOService::serviceMatching(gAgdcFBClass[ci]); if (!m) continue;
        IOService *f = IOService::copyMatchingService(m); m->release(); if (!f) continue;
#ifdef NMX_XEGFX
        // gAgdcFBClass[0] is the legacy IONDRVFramebuffer subclass and is matched exactly as before; the adapter class
        // comes second and only counts once it really is the live framebuffer (see nvdaAgdcAltEligible).
        if (ci != 0) {
            const char *why = nullptr;
            if (!nvdaAgdcAltEligible(f, &why)) {
                if (gAgdcAltLogged < 4) { gAgdcAltLogged++; FBLOG("AGDC: class %s candidate %s skipped: %s -- the legacy class keeps the head", gAgdcFBClass[ci], f->getName(), why); }
                f->release(); continue;
            }
        }
#endif
        IOService *p = f->getProvider(); int hops = 0;
        while (p && !p->metaCast("IOPCIDevice") && hops++ < 8) p = p->getProvider();
        if (!p || !p->metaCast("IOPCIDevice")) { f->release(); continue; }
#ifdef NMX_XEGFX
        { OSData *v = OSDynamicCast(OSData, p->getProperty("vendor-id"));
          if (!v || v->getLength() < 2 || *(const uint16_t *)v->getBytesNoCopy() != AGDC_VENDOR_ID) { f->release(); continue; } }
        FBLOG("AGDC: target framebuffer %s is class %s on %s", f->getName(), gAgdcFBClass[ci], p->getName());
        gAgdcTargetClass = gAgdcFBClass[ci];
#endif
        gAgdcFBRef = f; *fb = f; *pci = p;
    }
}

static int gAgdcScanLogged = 0;
#define SCANLOG(fmt, ...) do { if (gAgdcScanLogged < 8) { gAgdcScanLogged++; FBLOG("AGDC scan: " fmt " [%d/8]", ##__VA_ARGS__, gAgdcScanLogged); } } while (0)
static void nvdaAgdcScan(IOService *pci)
{
    nvdaAgdcDropTable(); gAgdcScanLogged = 0;
    IOService *tab[kAgdcMaxFB] = {}; unsigned seen = 0, noidx = 0, range = 0, dup = 0, other = 0, hole = 0, repeat = 0;
#ifdef NMX_XEGFX
    unsigned alt = 0;
#endif
    unsigned long long ids[16] = {}; unsigned nids = 0;
    bool any = false;
    // Both framebuffer classes are enumerated into one slot table; the IOFBDependentIndex rules below are unchanged.
    // The id list only keeps a node from being counted twice if a later class ever becomes a subclass of an earlier one.
    for (unsigned ci = 0; ci < AGDC_FB_CLASS_COUNT; ci++) {
        OSDictionary *m = IOService::serviceMatching(gAgdcFBClass[ci]);
        OSIterator *it = m ? IOService::getMatchingServices(m) : nullptr; OSSafeReleaseNULL(m);
        if (!it) { SCANLOG("class %s: no iterator", gAgdcFBClass[ci]); continue; }
        any = true;
        unsigned classSeen = 0;
        OSObject *o;
        while (seen < 16 && (o = it->getNextObject())) {
            IOService *f = OSDynamicCast(IOService, o); if (!f) continue;
            unsigned long long rid = f->getRegistryEntryID();
            bool known = false;
            for (unsigned k = 0; k < nids; k++) if (ids[k] == rid) { known = true; break; }
            if (known) { repeat++; continue; }
            seen++; classSeen++;
            if (nids < 16) ids[nids++] = rid;
#ifdef NMX_XEGFX
            if (ci != 0) {
                // never report the inert second framebuffer as a head: the legacy class keeps the table exactly as before
                const char *why = nullptr;
                if (!nvdaAgdcAltEligible(f, &why)) {
                    alt++; SCANLOG("class %s framebuffer id %#llx is not a live head (%s): left out", gAgdcFBClass[ci], rid, why);
                    continue;
                }
            }
#endif
            IOService *p = f->getProvider(); int hops = 0;
            while (p && !p->metaCast("IOPCIDevice") && hops++ < 8) p = p->getProvider();
            if (p != pci) { other++; SCANLOG("framebuffer id %#llx (class %s) is under another PCI device: not ours to report", rid, gAgdcFBClass[ci]); continue; }
            OSObject *io = f->copyProperty("IOFBDependentIndex"); OSNumber *num = OSDynamicCast(OSNumber, io);
            long long ix = num ? (long long)num->unsigned32BitValue() : -1; OSSafeReleaseNULL(io);
            if (ix < 0) { noidx++; SCANLOG("framebuffer id %#llx has no IOFBDependentIndex number: left out", rid); continue; }
            if (ix >= kAgdcMaxFB) { range++; SCANLOG("framebuffer id %#llx IOFBDependentIndex %lld >= %d: left out", rid, ix, (int)kAgdcMaxFB); continue; }
            if (tab[ix]) { dup++; SCANLOG("IOFBDependentIndex %lld claimed twice: id %#llx kept, id %#llx left out", ix, (unsigned long long)tab[ix]->getRegistryEntryID(), rid); continue; }
            f->retain(); tab[ix] = f;
        }
        it->release();
        SCANLOG("class %s: %u node(s) in this enumeration", gAgdcFBClass[ci], classSeen);
    }
    if (!any) { snprintf(gAgdcFBMap, sizeof gAgdcFBMap, "n=0 (no iterator: the earlier one-head reply)"); FBLOG("AGDC: slot table %s", gAgdcFBMap); return; }
    unsigned n = 0; while (n < kAgdcMaxFB && tab[n]) n++;
    for (unsigned i = n; i < kAgdcMaxFB; i++) if (tab[i]) {
        hole++; SCANLOG("slot %u (id %#llx) sits beyond a hole at slot %u: left out", i, (unsigned long long)tab[i]->getRegistryEntryID(), n);
        tab[i]->release(); tab[i] = nullptr;
    }
    for (unsigned i = 0; i < n; i++) gAgdcFB[i] = tab[i];
    gAgdcNFB = n;
    int w = snprintf(gAgdcFBMap, sizeof gAgdcFBMap, "n=%u", n);
    for (unsigned i = 0; i < n && w > 0 && (unsigned)w < sizeof gAgdcFBMap; i++)
        w += snprintf(gAgdcFBMap + w, sizeof gAgdcFBMap - (size_t)w, " [%u]id %#llx", i, (unsigned long long)gAgdcFB[i]->getRegistryEntryID());
    if (w > 0 && (unsigned)w < sizeof gAgdcFBMap) {
#ifdef NMX_XEGFX
        if (noidx | range | dup | other | hole | repeat | alt)
            snprintf(gAgdcFBMap + w, sizeof gAgdcFBMap - (size_t)w, " (left out: noidx %u range %u dup %u gpu %u hole %u repeat %u alt %u)", noidx, range, dup, other, hole, repeat, alt);
#else
        if (noidx | range | dup | other | hole | repeat)
            snprintf(gAgdcFBMap + w, sizeof gAgdcFBMap - (size_t)w, " (left out: noidx %u range %u dup %u gpu %u hole %u repeat %u)", noidx, range, dup, other, hole, repeat);
#endif
    }
    FBLOG("AGDC: slot table %s (%u framebuffer node(s) seen)", gAgdcFBMap, seen);
}

static void nvdaAgdcWorkBody(void);
static void nvdaAgdcWork(thread_call_param_t, thread_call_param_t)
{
    OSIncrementAtomic(&gAgdcBusy); nvdaAgdcWorkBody(); OSDecrementAtomic(&gAgdcBusy);
}
static void nvdaAgdcWorkBody(void)
{
    if (gAgdcWant == 1 && !gAgdc) {
        IOService *pci = nullptr, *fb = nullptr; nvdaAgdcTargets(&pci, &fb);
        if (!pci || !fb) { nvdaAgdcFailNote(-1, "no-eligible-framebuffer"); FBLOG("AGDC: no PCI device or no framebuffer yet (pci %p fb %p)", pci, fb); return; }
        nvdaAgdcScan(pci);
#ifdef NMX_XEGFX
        // a userspace-armed AGDC that came up before the accelerator's 6/6 gets its K5 half applied here
        if (gAgdcK5 == 0 && nvdaAgdcPropTrue(pci, "XeGfxAccelAgdcRequest")) {
            gAgdcK5 = 1; gAgdcRequestSeen = 1;
            FBLOG("AGDC auto: XeGfxAccelAgdcRequest on %s -> K5 enabled (AGDC was armed by userspace)", pci->getName());
        }
#endif
        NVDADeviceControl *d = OSTypeAlloc(NVDADeviceControl);
        if (!d) { nvdaAgdcFailNote(-2, "alloc-failed"); FBLOG("AGDC: alloc failed"); nvdaAgdcDropFB(); return; }
        if (!d->init(nullptr)) { nvdaAgdcFailNote(-3, "init-failed"); FBLOG("AGDC: init failed"); d->release(); nvdaAgdcDropFB(); return; }
        d->fPci = pci; d->fFB = fb;
        if (!d->attach(pci)) { FBLOG("AGDC: attach(%s) failed", pci->getName()); d->release(); nvdaAgdcDropFB(); nvdaAgdcFailNote(-4, "attach-failed"); return; }
        if (!d->start(pci)) { FBLOG("AGDC: AppleGraphicsDeviceControl::start failed (vendor calls so far %lld)", gAgdcCmds); d->detach(pci); d->release(); nvdaAgdcDropFB(); nvdaAgdcFailNote(-5, "agdc-start-failed"); return; }
        gAgdc = d; nvdaAgdcFailNote(1, "none");
        FBLOG("AGDC: UP on %s — %u framebuffer slot(s), maxFramebuffers %d; vendor calls %lld, refused %lld (last %#x)", pci->getName(), gAgdcNFB, gAgdcMaxFBOut, gAgdcCmds, gAgdcUnsupported, gAgdcLastRefused);
    } else if (gAgdcWant == 0 && gAgdc) {
        NVDADeviceControl *d = gAgdc; gAgdc = nullptr;
        bool ok = d->terminate(kIOServiceRequired | kIOServiceSynchronous); d->release(); nvdaAgdcDropFB(); nvdaAgdcFailNote(2, "terminated");
        FBLOG("AGDC: terminate -> %d (a reboot is the certain way back; this is the polite one)", ok);
    }
}
static int nvrmfb_agdc_sysctl SYSCTL_HANDLER_ARGS
{
    int v = gAgdcState; int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (v != 0 && v != 1) return EINVAL;
#ifdef NMX_XEGFX
    // a terminated client on the live iGPU is the FLR risk the XeGfx notes warn about: up only, a reboot takes it down
    if (v == 0) return EPERM;
#endif
    gAgdcWant = v;
    if (!gAgdcCall) gAgdcCall = thread_call_allocate(nvdaAgdcWork, nullptr);
    if (!gAgdcCall) return ENOMEM;
#ifdef NMX_XEGFX
    nvdaAgdcAutoNoteManual();
#endif
    thread_call_enter(gAgdcCall);
    nvdaAgdcPublish();
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, nvrmfb_agdc, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL, 0, nvrmfb_agdc_sysctl, "I",
            "write 1 = create our AppleGraphicsDeviceControl object, 0 = terminate it; reads the state (1 UP, <0 failed step)");
SYSCTL_QUAD(_debug, OID_AUTO, nvrmfb_agdc_cmds,    CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcCmds,        "AGDC vendor calls received");
SYSCTL_QUAD(_debug, OID_AUTO, nvrmfb_agdc_refused, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcUnsupported, "AGDC vendor calls answered Unsupported");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_last_refused, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcLastRefused, 0, "last AGDC command refused");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_k5,      CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gAgdcK5,     0, "K5: 1 = answer kAGDCLinkConfig + plane-scaler capabilities (0 after every boot)");
SYSCTL_QUAD(_debug, OID_AUTO, nvrmfb_agdc_k5_link, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5Link,    "K5: kAGDCLinkConfig queries answered");
SYSCTL_QUAD(_debug, OID_AUTO, nvrmfb_agdc_k5_caps, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5Caps,    "K5: plane-scaler capability queries answered");
SYSCTL_INT (_debug, OID_AUTO, xegfx_agdc_k5_colour, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &nvk5_colour_on, 0, "§37.12: 1 = answer the colour-stage capabilities (CSC, tables); read by WindowServer when it starts");
SYSCTL_INT (_debug, OID_AUTO, xegfx_agdc_k5_bgra, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &nvk5_bgra_on, 0, "§37.13: 1 = the plane capability lists BGRA (CoreDisplay can scan WindowServer's composite directly); read when WindowServer starts");
SYSCTL_QUAD(_debug, OID_AUTO, xegfx_agdc_k5_colour_answers, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5Colour, "§37.12: colour-stage capability queries answered");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_k5_why,  CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5Why,  0, "K5: why the last K5 query was not answered");
SYSCTL_STRING(_debug, OID_AUTO, nvrmfb_agdc_fbmap, CTLFLAG_RD | CTLFLAG_LOCKED, gAgdcFBMap, sizeof gAgdcFBMap, "the slot table our 0x980 reports: n, then [i]id per IOFBDependentIndex");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_client_ack, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gAgdcClientAck, 0, "1 = accept WindowServer's 0x2001 client set");
SYSCTL_QUAD(_debug, OID_AUTO, nvrmfb_agdc_client_set, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcClientSet, "0x2001 client-set calls accepted");
SYSCTL_QUAD(_debug, OID_AUTO, nvrmfb_agdc_rescans, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcRescans, "slot-table rescans done while answering 0x980 (a framebuffer that started after boot)");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_maxfb,   CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcMaxFBOut, 0, "maxFramebuffers in our last 0x980 reply (0 = never asked)");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_k5_asked, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5Asked, 0, "bit i = a 0x921/0x711 named index i");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_k5_link_mask, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5LinkMask, 0, "bit i = kAGDCLinkConfig answered for index i");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_k5_caps_mask, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5CapsMask, 0, "bit i = the plane-scaler capability answered for index i");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_k5_last_refused_idx, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5LastRefusedIdx, 0, "the index the last BADARG/NOTFOUND refusal named (-1 none); an Unsupported 0x711 type shows in k5_asked only");
static void nvdaAgdcRemoveSysctl(void)
{
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc); sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_cmds);
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_refused); sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_last_refused);
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_k5); sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_k5_link);
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_k5_caps); sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_k5_why);
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_fbmap); sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_maxfb);
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_rescans);
    sysctl_unregister_oid(&sysctl__debug_xegfx_agdc_k5_bgra);
    sysctl_unregister_oid(&sysctl__debug_xegfx_agdc_k5_colour); sysctl_unregister_oid(&sysctl__debug_xegfx_agdc_k5_colour_answers);
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_k5_asked); sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_k5_link_mask);
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_k5_caps_mask); sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_k5_last_refused_idx);
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_client_ack); sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_client_set);
}
static void nvdaAgdcInstallSysctl(void)
{
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc); sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_cmds);
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_refused); sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_last_refused);
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_k5); sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_k5_link);
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_k5_caps); sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_k5_why);
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_fbmap); sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_maxfb);
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_rescans);
    sysctl_register_oid(&sysctl__debug_xegfx_agdc_k5_bgra);
    sysctl_register_oid(&sysctl__debug_xegfx_agdc_k5_colour); sysctl_register_oid(&sysctl__debug_xegfx_agdc_k5_colour_answers);
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_k5_asked); sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_k5_link_mask);
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_k5_caps_mask); sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_k5_last_refused_idx);
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_client_ack); sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_client_set);
}

class NVDAGDCLoader : public IOService {
    OSDeclareDefaultStructors(NVDAGDCLoader)
public:
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(NVDAGDCLoader, IOService)
bool NVDAGDCLoader::start(IOService *provider)
{
#ifndef NMX_XEGFX
    { if (nvrm_driver_off()) return false; }
#endif
    if (!IOService::start(provider)) return false;
    { int v = 0; if (PE_parse_boot_argn("xegfxagdcnocolour", &v, sizeof v)) nvk5_colour_on = 0; }   // §37.12
    { int v = 0; if (PE_parse_boot_argn("xegfxagdcnobgra", &v, sizeof v)) nvk5_bgra_on = 0; }       // §37.13
    nvdaAgdcInstallSysctl();
#ifdef NMX_XEGFX
    // M3d: the accelerator's XeGfxAccelAgdcRequest (set on the GPU node at boot-armed 6/6) arms AGDC automatically;
    // the two userspace sysctls keep working and take precedence if written first.
    nvdaAgdcAutoStart(this);
    FBLOG("loaded; the accelerator arms AGDC automatically (XeGfxAccelAgdcRequest); `sysctl debug.%s=1` still works and takes precedence", sysctl__debug_nvrmfb_agdc.oid_name);
#else
    FBLOG("loaded; idle until `sysctl debug.%s=1`", sysctl__debug_nvrmfb_agdc.oid_name);
#endif
    return true;
}
void NVDAGDCLoader::stop(IOService *provider)
{
#ifdef NMX_XEGFX
    nvdaAgdcAutoStop(); gAgdcLoaderSvc = nullptr;
#endif
    nvdaAgdcRemoveSysctl();
    if (gAgdcCall) {
        thread_call_cancel(gAgdcCall); int spins = 0;
        while (gAgdcBusy && spins++ < 200) IOSleep(10);
        if (gAgdcBusy) FBLOG("stop: the worker is STILL running after 2 s — leaving its thread call allocated rather than freeing it under it");
        else { thread_call_free(gAgdcCall); gAgdcCall = nullptr; }
    }
    if (gAgdc && !gAgdcBusy) { gAgdcWant = 0; nvdaAgdcWorkBody(); }
    IOService::stop(provider);
}
