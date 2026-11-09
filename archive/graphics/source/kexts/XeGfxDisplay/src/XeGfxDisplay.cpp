/*
 * XeGfxDisplay.cpp -- NVRMFB-style IOFramebuffer adapter over the XeGfx KMS
 *                     engine (i915 display).
 *
 * Status: skeleton with the full method surface implemented or stubbed per
 * phase.  Every function is labelled with the milestone that makes it real:
 *
 *   M1  discovery only      -- getCaps/getConnectors, log, no hardware change
 *   M2  mode table          -- publish connector modes, still the fw raster
 *   M3  takeover            -- real modeset, vblank, cursor, flips, accel bind
 *   M4  240Hz + DSC         -- commit the high-refresh mode, DSC params
 *   M5  HiDPI               -- scaled modes through the pipe scaler
 *
 * Anything not yet real returns a safe value and logs once, so a partial
 * build can still boot and be inspected.
 */

#include "XeGfxDisplay.h"

#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>          /* PE_parse_boot_argn                */
#include <IOKit/IODeviceTreeSupport.h> /* gIODTPlane (NVRAM /options)     */
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <string.h>                   /* memcpy/strlen/strlcpy (kernel)    */
#include <mach/kmod.h>                /* kmod_info_t / _kmod_info symbol   */
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSSymbol.h>
#include <IOKit/graphics/IODisplay.h>   /* B1: IODisplayParameterHandler, brightness keys */
#include <kern/thread.h>                /* E3: kernel_thread_start            */
#include <mach/thread_act.h>            /* E3: thread_terminate               */

/* IOFramebuffer keeps a detailed timing's mode id in the first reserved word (as NVRMFB names it) */
#ifndef detailedTimingModeID
#define detailedTimingModeID __reservedA[0]
#endif

/* A1: the accepted-timing comparison against one reference (defined with timingMatchesAccepted) */
static bool xgDtMatches(const IODetailedTimingInformationV2 &a, const IODetailedTimingInformationV2 *dt);
static inline bool xgDtMatchesBase(const IODetailedTimingInformationV2 &a, const IODetailedTimingInformationV2 *dt)
{
    return xgDtMatches(a, dt);
}

/* M3d: defined in the helper section; readKapi() runs long before it. */
static bool xgKernelPointerOk(uint64_t ptr);
static const OSSymbol *xgSymbol(const char *name, const OSSymbol **cache);


/*
 * kmutil places a kext in a kernel collection only with a _kmod_info symbol
 * (Xcode generates it; this direct-clang build does not).  Same shim as the
 * XeGfxIntelFB kext: a plain kmod entry point pair with no work to do, since
 * the IOKit personalities are what start the driver.
 */
extern "C" kern_return_t _start(kmod_info_t *ki, void *d);
extern "C" kern_return_t _stop(kmod_info_t *ki, void *d);
extern "C" { KMOD_EXPLICIT_DECL(org.watchthelight.XeGfxDisplay, "0.5.45", _start, _stop) }
extern "C" kern_return_t _start(kmod_info_t *ki, void *d) { return KERN_SUCCESS; }
extern "C" kern_return_t _stop(kmod_info_t *ki, void *d)  { return KERN_SUCCESS; }

#define XLOG(fmt, ...) \
    IOLog(XEGFX_DISPLAY_LOG_PREFIX fmt "\n", ##__VA_ARGS__)

OSDefineMetaClassAndStructors(XeGfxFramebuffer, IOFramebuffer)
OSDefineMetaClassAndStructors(XeGfxIntelKMSFramebuffer, XeGfxFramebuffer)

/* ======================================================================= */
/* boot-arg / NVRAM helpers                                                */
/* ======================================================================= */

static bool
xgBootArgEnabled(const char *name, bool dflt)
{
    /*
     * PE_parse_boot_argn() has accepted both "name=value" and "-name=value"
     * spellings over the years; the XeGfxIntelFB placeholder relies on the
     * dashed form ("-xeintelfbhidpi"), while docs/04-i915-INTEGRATION.md
     * measured the undashed form.  Try the undashed name first (that is the
     * form the M2b acceptance test uses), then the dashed spelling, so a
     * literal "-xedisp=1" works on every supported XNU.
     *
     * The value must be parsed by PE_parse_boot_argn(): a bare "-xedisp" (no
     * '=') leaves val at 1, "-xedisp=0" disables.  Never pass the whole
     * "-xedisp=0" string as the name (P1-19): the name stops at '='.
     */
    int val = 1;
    if (PE_parse_boot_argn(name, &val, sizeof val))
        return val != 0;

    if (name[0] != '-') {
        char dashed[48];
        size_t len = strlen(name);
        if (len + 2 <= sizeof dashed) {
            dashed[0] = '-';
            memcpy(dashed + 1, name, len + 1);
            val = 1;
            if (PE_parse_boot_argn(dashed, &val, sizeof val))
                return val != 0;
        }
    }
    return dflt;
}

/* ======================================================================= */
/* M3c S1: NVRAM gates                                                     */
/* ======================================================================= */
/*
 * The takeover gates live in the device tree "/options" node, NOT in
 * boot-args: OpenCore rewrites boot-args on every boot (measured 2026-10-27),
 * so a boot-arg switch cannot survive a restart.  nvram(8) stores numbers as
 * 4-byte little-endian OSData.
 *
 *   xegfx-disp-phase  0 = off (M2b skeleton), 1 = inert registration (S2),
 *                     2 = same-mode claim (S3)
 *   xegfx-disp-trial  1 = allow XeGfxFBClaim to outbid XeGfxIntelFB (S3)
 */
static bool
xgNvramU32(const char *name, uint32_t *out)
{
    *out = 0;
    IORegistryEntry *options =
        IORegistryEntry::fromPath("/options", gIODTPlane);
    if (!options)
        return false;
    OSData *data = OSDynamicCast(OSData, options->getProperty(name));
    bool ok = false;
    if (data && data->getLength() >= 1 && data->getLength() <= sizeof(uint32_t)) {
        /* nvram(8) may store the value as 1..4 bytes (e.g. "%01"); decode LE. */
        const uint8_t *b = (const uint8_t *)data->getBytesNoCopy();
        uint32_t v = 0;
        for (unsigned int i = 0; i < data->getLength(); i++)
            v |= (uint32_t)b[i] << (8 * i);
        *out = v;
        ok = true;
    }
    options->release();
    return ok;
}

/*
 * M3c S3: write a gate back to NVRAM.  This is the ONLY write path in the
 * kext, it is only reached from the S3 watchdog, and it touches nothing but
 * /options.  nvram(8) reads integers as 1..4 little-endian bytes; always
 * store the canonical 4-byte form so a later phase can trust the width.
 *
 * Clearing a gate is an explicit write of 0: removeProperty() on /options
 * does not persist to the NVRAM store (measured on the XeGfxIntelFB guard,
 * docs/08 N-283), so "remove" would silently leave the old value behind.
 */
static bool
xgNvramSetU32(const char *name, uint32_t value)
{
    IORegistryEntry *options = IORegistryEntry::fromPath("/options", gIODTPlane);
    if (!options) {
        XLOG("nvram: /options not found; %s=%u NOT written", name, value);
        return false;
    }

    uint8_t bytes[sizeof(uint32_t)];
    for (unsigned int i = 0; i < sizeof bytes; i++)
        bytes[i] = (uint8_t)(value >> (8 * i));

    OSData *data = OSData::withBytes(bytes, sizeof bytes);
    bool ok = false;
    if (data) {
        ok = options->setProperty(name, data);
        data->release();
    } else {
        XLOG("nvram: OSData allocation failed; %s=%u NOT written", name, value);
    }
    options->release();

    if (!ok)
        XLOG("nvram: setProperty(%s, %u) failed -- NVRAM unchanged", name, value);
    return ok;
}

/*
 * M2b: read a numeric nub property without assuming how the core typed it.
 * The M1b nub uses IOService::setProperty(name, uint64, 32) (an OSNumber),
 * but OSData is accepted too so the reader survives the M2c core change.
 */
static uint32_t
xgUIntProperty(IOService *provider, const char *key, uint32_t dflt)
{
    if (!provider)
        return dflt;

    OSObject *prop = provider->getProperty(key);
    if (OSNumber *num = OSDynamicCast(OSNumber, prop))
        return num->unsigned32BitValue();

    if (OSData *data = OSDynamicCast(OSData, prop)) {
        const void *bytes = data->getBytesNoCopy();
        switch (data->getLength()) {
        case 1:  return *(const uint8_t *)bytes;
        case 2:  return *(const uint16_t *)bytes;
        case 4:  return *(const uint32_t *)bytes;
        case 8:  return (uint32_t)(*(const uint64_t *)bytes);
        default: break;
        }
    }
    return dflt;
}

/*
 * Start counter, same purpose as NVRMFB's "xeintelfb-starts": a modeset that
 * hangs leaves the machine only reachable in single-user mode, so after
 * XEGFX_DISPLAY_MAX_STARTS counted starts we stop taking the display over and
 * let the placeholder framebuffer keep the firmware raster alive.
 */
static bool
xgStartsExhausted(void)
{
    /*
     * gIODTPlane is private to IOKit and not in the Kernel.framework headers,
     * so look the plane up by name.  fromPath() returns OSPtr<>; with
     * IOKIT_ENABLE_SHARED_PTR unset that is a raw +1 reference, so release it
     * on every path out of this function.
     */
    const IORegistryPlane *plane = IORegistryEntry::getPlane("IODeviceTree");
    if (!plane)
        return false;
    IORegistryEntry *options = IORegistryEntry::fromPath("/options", plane);
    if (!options)
        return false;
    OSData *d = OSDynamicCast(OSData, options->getProperty(XEGFX_DISPLAY_NVRAM_STARTS));
    unsigned n = 0;
    if (d && d->getLength() >= sizeof n)
        n = *(const unsigned *)d->getBytesNoCopy();
    options->release();
    return n >= XEGFX_DISPLAY_MAX_STARTS;
}

/* ======================================================================= */
/* M1: attach, read the KAPI table, snap the connector                     */
/* ======================================================================= */

bool
XeGfxFramebuffer::readKapi(IOService *provider)
{
    /*
     * M3d: the M3b core publishes the v1 KAPI as a single-pointer property
     * ("xegfx-kms-kapi" = OSData holding one uint64).  The M2c draft parsed a
     * struct copy out of that property; that ABI is gone, and the old parse
     * is why every non-S3 boot silently fell through to the skeleton path.
     *
     * Validate before the first dereference: a stale/wrong pointer here is a
     * kernel panic, not a bad mode.
     */
    fKapi            = nullptr;
    fCtx             = nullptr;
    fKapiFlags       = 0;
    fKapiTimingReady = false;

    OSData *kapiData =
        OSDynamicCast(OSData, provider->getProperty("xegfx-kms-kapi"));
    if (!kapiData) {
        XLOG("kapi: nub has no xegfx-kms-kapi property");
        return false;
    }
    if (kapiData->getLength() != sizeof(uint64_t) ||
        !kapiData->getBytesNoCopy()) {
        XLOG("kapi: property is %u bytes, expected 8 (pointer ABI)",
             (unsigned)kapiData->getLength());
        return false;
    }

    uint64_t ptr = *(const uint64_t *)kapiData->getBytesNoCopy();
    if (!xgKernelPointerOk(ptr)) {
        XLOG("kapi: refused property pointer 0x%llx (not a kernel image "
             "address)", (unsigned long long)ptr);
        return false;
    }

    const struct XeGfxKmsKapi *k =
        (const struct XeGfxKmsKapi *)(uintptr_t)ptr;
    if (k->magic != XEGFX_KMS_KAPI_MAGIC) {
        XLOG("kapi: bad magic 0x%x (want 0x%x)",
             k->magic, XEGFX_KMS_KAPI_MAGIC);
        return false;
    }
    if (k->version != XEGFX_KMS_KAPI_VERSION ||
        k->size != XEGFX_KMS_KAPI_SIZE) {
        XLOG("kapi: v%u size %u is not the v1 136-byte table this adapter "
             "speaks", k->version, k->size);
        return false;
    }
    if (!xgKernelPointerOk((uint64_t)(uintptr_t)k->getTiming) ||
        !xgKernelPointerOk((uint64_t)(uintptr_t)k->getConnectorInfo)) {
        XLOG("kapi: required entry points NULL (timing %p connector %p)",
             (const void *)k->getTiming, (const void *)k->getConnectorInfo);
        return false;
    }

    fKapi      = k;
    fCtx       = k->ctx;
    fKapiFlags = k->flags;
    XLOG("kapi: v1 table %p ctx %p flags 0x%x (read-only=%d scanout-ext=%d)",
         (const void *)k, fCtx, fKapiFlags,
         !!(fKapiFlags & XEGFX_KMS_KAPI_FLAG_READ_ONLY),
         !!(fKapiFlags & XEGFX_KMS_KAPI_FLAG_SCANOUT));
    return true;
}

bool
XeGfxFramebuffer::kapiInit()
{
    if (!fKapi)
        return false;

    bzero(&fConn, sizeof fConn);
    fHandle       = 0;
    fHead         = 4;      /* M3d/accel display head, fixed by the contract */
    fOwnsHead     = false;
    fIsConsole    = false;

    /* ---- connector: first VBT child that the core knows about ---------- */
    struct XeGfxKmsConnectorInfo info;
    bzero(&info, sizeof info);
    info.size = (uint32_t)sizeof info;

    uint32_t rc = XEGFX_ERR_NOT_FOUND;
    for (uint32_t i = 0; i < XEGFX_KMS_MAX_CONNECTORS; i++) {
        bzero(&info, sizeof info);
        info.size = (uint32_t)sizeof info;
        rc = fKapi->getConnectorInfo(fCtx, i, &info);
        if (rc == XEGFX_OK)
            break;
    }
    if (rc == XEGFX_OK) {
        fConn.index         = info.index;
        fConn.handle        = info.handle;
        fConn.type          = info.type;
        fConn.panelWidthMm  = (uint16_t)info.panelWidthMm;
        fConn.panelHeightMm = (uint16_t)info.panelHeightMm;
        fConn.headMask      = 1u << fHead;
        if (info.flags & XEGFX_CONN_FLAG_INTERNAL) {
            fConn.caps |= XEGFX_CONN_CAP_INTERNAL;
        }
        if ((fConn.caps & XEGFX_CONN_CAP_INTERNAL) || info.type == XEGFX_CONN_DP) {
            fOwnsHead  = true;
            fIsConsole = true;
        }
        XLOG("kapi: connector %u handle 0x%x type %u %ux%u mm flags 0x%x "
             "(internal=%d)", fConn.index, fConn.handle, fConn.type,
             fConn.panelWidthMm, fConn.panelHeightMm, info.flags,
             !!(fConn.caps & XEGFX_CONN_CAP_INTERNAL));
    } else {
        XLOG("kapi: no connector info (last rc 0x%x); panel facts unavailable",
             rc);
    }

    /* ---- timing: the engine's current raster as 13 NVK5 words ---------- */
    struct XeGfxKmsPanelTiming pt;
    bzero(&pt, sizeof pt);
    rc = fKapi->getTiming(fCtx, fIndex, &pt);
    if (rc == XEGFX_OK && pt.tag == XEGFX_NVK5_TIMING_TAG) {
        memcpy(fKapiTimingWords, &pt, sizeof pt);
        fKapiTimingReady = true;
        if (!publishAgdcTiming())
            return false;
        XLOG("kapi: timing READY %ux%u tot %ux%u pclk %u Hz refresh %u mHz "
             "hsync+%u vsync+%u", pt.hVisible, pt.vVisible, pt.hTotal,
             pt.vTotal, pt.pixelClockHz, pt.refreshMilliHz, pt.hSyncPositive,
             pt.vSyncPositive);
    } else {
        XLOG("kapi: getTiming(%u) not READY (rc 0x%x tag 0x%x)",
             fIndex, rc, pt.tag);
    }

    setProperty("XeGfxKapiVersion", (unsigned long long)XEGFX_KMS_KAPI_VERSION, 32);
    return true;
}

/* ======================================================================= */
/* M2: mode table                                                          */
/* ======================================================================= */

void
XeGfxFramebuffer::timingToIo(const struct XeGfxKmsTiming *t,
                             IODetailedTimingInformationV2 *dt) const
{
    bzero(dt, sizeof *dt);

    dt->pixelClock       = (UInt64)t->clockKHz * 1000ull;
    /*
     * NVRMFB sets min == max on purpose: setVBLTiming() in IOFramebuffer
     * clears the pixel count when min != max, which throws away the VBL
     * delta and makes CoreDisplay's frame pacing drift.  Keep them equal.
     */
    dt->minPixelClock    = dt->pixelClock;
    dt->maxPixelClock    = dt->pixelClock;

    dt->horizontalActive       = t->hdisplay;
    dt->horizontalBlanking     = t->htotal - t->hdisplay;
    dt->horizontalSyncOffset   = t->hsyncStart - t->hdisplay;
    dt->horizontalSyncPulseWidth = t->hsyncEnd - t->hsyncStart;

    dt->verticalActive         = t->vdisplay;
    dt->verticalBlanking       = t->vtotal - t->vdisplay;
    dt->verticalSyncOffset     = t->vsyncStart - t->vdisplay;
    dt->verticalSyncPulseWidth = t->vsyncEnd - t->vsyncStart;

    dt->horizontalBorderLeft = dt->horizontalBorderRight = 0;
    dt->verticalBorderTop    = dt->verticalBorderBottom = 0;

    dt->horizontalSyncConfig = (t->flags & XEGFX_TF_HSYNC_POS)
                                   ? kIOSyncPositivePolarity : 0;
    dt->verticalSyncConfig   = (t->flags & XEGFX_TF_VSYNC_POS)
                                   ? kIOSyncPositivePolarity : 0;
    dt->horizontalSyncLevel = dt->verticalSyncLevel = 0;

    dt->signalConfig = kIODigitalSignal;
    dt->signalLevels = 0;
    dt->numLinks     = (t->pixelEncoding == 2) ? 2 : 1;   /* YCbCr422 dual  */

    dt->pixelEncoding        = t->pixelEncoding;
    dt->bitsPerColorComponent= t->bpc;
    dt->colorimetry          = t->colorimetry;
    dt->dynamicRange         = t->dynamicRange;

    /* M4: DSC is part of the IOFB timing struct, so CoreDisplay learns about
     * it directly instead of us hiding it. */
    if (t->dsc) {
        dt->dscCompressedBitsPerPixel = 8;
        dt->dscSliceHeight = 16;
        dt->dscSliceWidth  = (t->hdisplay >= 1280) ? 2 * (t->hdisplay / 2 / 2) : 0;
        if (dt->dscSliceWidth == 0 || dt->dscSliceWidth > 4096)
            dt->dscSliceWidth = 1280;
    }

    if (t->flags & XEGFX_TF_INTERLACED) {
        /* IOFB expresses interlacing through the pixel count only; keep the
         * flag in the caller's IOTimingInformation.flags instead. */
        dt->verticalActive = t->vdisplay;
    }

    /* HiDPI: nominal smaller than raster is expressed through the scaled
     * fields.  WindowServer then allocates nominal*2 and lets the pipe
     * scaler compress (M5). */
    if (t->hdisplay && t->hdisplay < 0) { /* unreachable; keeps the field list */
    }
}

IOReturn
XeGfxFramebuffer::getTimingInfoForDisplayMode(IODisplayModeID mode,
                                              IOTimingInformation *info)
{
    /*
     * S3: hand back the measured record verbatim.  This is the same timing
     * that validateDetailedTiming() accepts and enableController() publishes,
     * so all three answers agree by construction (docs/12 §4.2 check 1).
     */
    if (fS3) {
        if (!info)
            return kIOReturnBadArgument;
        int si = s3Index(mode);
        if ((!fFbModeReady && !(fM3d && fM3dChecks == XEGFX_M3D_CHECKS_ALL)) || si < 0)
            return kIOReturnUnsupported;
        bzero(info, sizeof *info);
        info->appleTimingID = kIOTimingIDInvalid;
        info->flags         = kIODetailedTimingValid;
        if (si >= XEGFX_A1_DT_INDEX) {
            info->flags |= kIOScalingInfoValid;
            info->detailedInfo.v2 = fDT[si - XEGFX_A1_DT_INDEX];
        } else
            info->detailedInfo.v2 = si == 0 ? fAcceptedDt : fMeasuredModes[si].dt;
        return kIOReturnSuccess;
    }

    int idx = findModeByModeId(mode);
    if (idx < 0 || !info)
        return kIOReturnBadArgument;

    const struct XeGfxKmsMode *m = &fConn.modes[idx];
    bzero(info, sizeof *info);
    info->appleTimingID = kIOTimingIDInvalid;
    info->flags         = kIODetailedTimingValid;
    timingToIo(&m->t, &info->detailedInfo.v2);

    /*
     * M5: when the mode is a scaled one, report the *scaled* (nominal) size so
     * CoreDisplay allocates nominal*2 and the plane reads it at that size.
     */
    if (m->flags & XEGFX_MODE_SCALED) {
        info->flags |= kIOScalingInfoValid;
        info->detailedInfo.v2.horizontalScaled = m->nominalWidth;
        info->detailedInfo.v2.verticalScaled   = m->nominalHeight;
        info->detailedInfo.v2.scalerFlags      = kIOScaleStretchToFit;
    }
    return kIOReturnSuccess;
}

IOReturn
XeGfxFramebuffer::getInformationForDisplayMode(IODisplayModeID mode,
                                               IODisplayModeInformation *info)
{
    if (fS3) {
        if (!info)
            return kIOReturnBadArgument;
        int si = s3Index(mode);
        if ((!fFbModeReady && !(fM3d && fM3dChecks == XEGFX_M3D_CHECKS_ALL)) || si < 0)
            return kIOReturnUnsupported;
        if (si >= XEGFX_A1_DT_INDEX) {
            const IODetailedTimingInformationV2 &d = fDT[si - XEGFX_A1_DT_INDEX];
            *info = fMeasuredModes[fDtBase[si - XEGFX_A1_DT_INDEX]].info;
            info->nominalWidth  = d.horizontalScaled;
            info->nominalHeight = d.verticalScaled;
            info->flags |= kDisplayModeSafeFlag | kDisplayModeNotPresetFlag;
        } else
            *info = fMeasuredModes[si].info;
        return kIOReturnSuccess;
    }

    int idx = findModeByModeId(mode);
    if (idx < 0 || !info)
        return kIOReturnBadArgument;

    const struct XeGfxKmsMode *m = &fConn.modes[idx];
    bzero(info, sizeof *info);

    info->nominalWidth  = m->nominalWidth;
    info->nominalHeight = m->nominalHeight;
    /* 16.16 fixed point, as IOFramebuffer documents. */
    info->refreshRate   = ((UInt32)m->refreshMHz << 16) / 1000u;
    info->maxDepthIndex = 0;

    /* EDID's physical size, so the desktop's DPI is right. */
    info->imageWidth   = (UInt16)fConn.panelWidthMm;
    info->imageHeight  = (UInt16)fConn.panelHeightMm;
    info->flags        = kDisplayModeValidFlag;
    return kIOReturnSuccess;
}

IOItemCount
XeGfxFramebuffer::getDisplayModeCount()
{
    /*
     * The single most expensive lesson from NVRMFB (nvrm-fb.cpp:436): macOS
     * builds the whole mode list from this call, BEFORE setDetailedTimings(),
     * and never re-reads it.  Everything the panel can do at every refresh
     * rate therefore has to be answerable from here, on the first call.
     */
    /*
     * M3c S3: exactly one mode, and only once enableController() has published
     * XeGfxFbModeReady.  Before that the FB must look empty: reporting a mode
     * early would let CoreDisplay validate/use a raster whose handshake has
     * not happened, and there is no aperture to scan it out anyway.
     */
    if (fS3) {
        /* One entry, from the measured table -- not from the KAPI table,
         * which may advertise several modes this step deliberately hides. */
        return (fMeasuredModeCount > 0 &&
            (fFbModeReady || (fM3d && fM3dChecks == XEGFX_M3D_CHECKS_ALL))) ? 1 + (fK3 ? fK3Count : 0) + fNDT : 0;
    }
    if (fInert)
        return 0;
    return fIoModeCount;
}

IOReturn
XeGfxFramebuffer::getDisplayModes(IODisplayModeID *allDisplayModes)
{
    if (!allDisplayModes)
        return kIOReturnBadArgument;
    if (fS3) {
        if (!fFbModeReady && !(fM3d && fM3dChecks == XEGFX_M3D_CHECKS_ALL))
            return kIOReturnUnsupported;
        allDisplayModes[0] = fMeasuredModes[0].modeId;
        uint32_t n = 1;
        for (uint32_t i = 0; fK3 && i < fK3Count; i++)
            allDisplayModes[n++] = fMeasuredModes[2 + i].modeId;
        for (uint32_t i = 0; i < fNDT; i++)
            allDisplayModes[n++] = (IODisplayModeID)fDT[i].detailedTimingModeID;
        return kIOReturnSuccess;
    }
    for (IOItemCount i = 0; i < fIoModeCount; i++)
        allDisplayModes[i] = fIoModeIds[i];
    return kIOReturnSuccess;
}

int
XeGfxFramebuffer::findModeByModeId(IODisplayModeID modeId) const
{
    if (fS3) {
        /* The S3 table is pinned to the one measured raster, mode id 1. */
        if (fFbModeReady || (fM3d && fM3dChecks == XEGFX_M3D_CHECKS_ALL))
            return s3Index(modeId);
        return -1;
    }
    for (IOItemCount i = 0; i < fIoModeCount; i++)
        if (fIoModeIds[i] == modeId)
            return (int)fIoCfgModeIdx[i];
    return -1;
}

bool
XeGfxFramebuffer::buildModeTable()
{
    /*
     * One IODisplayModeID per connector mode.  IDs start at 1 and are stable
     * for the life of the instance; fIoCfgModeIdx maps them back to
     * fConn.modes[].
     */
    if (fConn.numModes == 0)
        return false;
    fIoModeCount = fConn.numModes;
    fIoModeIds = (IODisplayModeID *)IOMalloc(fIoModeCount * sizeof *fIoModeIds);
    fIoCfgModeIdx = (uint32_t *)IOMalloc(fIoModeCount * sizeof *fIoCfgModeIdx);
    if (!fIoModeIds || !fIoCfgModeIdx)
        return false;

    for (IOItemCount i = 0; i < fIoModeCount; i++) {
        fIoModeIds[i]    = (IODisplayModeID)(i + 1);
        fIoCfgModeIdx[i] = i;
    }
    fCurrentIdx = fConn.currentMode;
    fCurrentId  = fIoModeIds[fCurrentIdx];
    logModes();
    return true;
}

void
XeGfxFramebuffer::logModes() const
{
    for (uint32_t i = 0; i < fConn.numModes; i++) {
        const struct XeGfxKmsMode *m = &fConn.modes[i];
        XLOG("  mode %u: %ux%u@%u.%03u (nominal %ux%u) clk %u kHz bpc %u "
             "dsc %u flags 0x%x",
             i, m->t.hdisplay, m->t.vdisplay,
             m->refreshMHz / 1000, m->refreshMHz % 1000,
             m->nominalWidth, m->nominalHeight, m->t.clockKHz,
             m->t.bpc, m->t.dsc, m->flags);
    }
}

/* ======================================================================= */
/* M3a: measured-panel-timing -> read-only IOFramebuffer mode records      */
/* ======================================================================= */

/*
 * The 13 words are the NVK5 timing record (see XEGFX_NVK5_T_* in the header):
 *
 *   0  tag 0x4b350001
 *   1  pixel clock (Hz)          = htotal*vtotal*refreshHz  (measured)
 *   2  h visible                 (2560 for this panel)
 *   3  h sync start              (2608)
 *   4  h sync end                (2640)
 *   5  h total                   (2720)
 *   6  v visible                 (1600)
 *   7  v sync start              (1606)
 *   8  v sync end                (1612)
 *   9  v total                   (1700)
 *  10  refresh, milli-Hz         (59856)
 *  11  hsync polarity (1 = +)    (1)
 *  12  vsync polarity (1 = +)    (0)
 *
 * Word 1 comes from a frame-counter measurement (the core derives it from
 * htotal*vtotal*fps, not from the PLL), so it can sit a fraction of a percent
 * below the EDID clock: 277,440,000 Hz nominal -> 276,774,144 Hz measured.
 * M3a reports the measured values; they are what the pipe will actually run.
 *
 * Everything here is local state.  No IOFramebuffer is registered, no
 * enableController(), no KMS call, no register access.
 */

bool
XeGfxFramebuffer::parseNvk5Timing(const uint32_t *t, uint32_t words,
                                  struct XeGfxKmsTiming *out,
                                  uint64_t *pixelClockHz,
                                  uint32_t *refreshMHz,
                                  const char **why)
{
    if (why)
        *why = "";
    if (!t || !out || !pixelClockHz || !refreshMHz) {
        if (why) *why = "null argument";
        return false;
    }
    if (words != XEGFX_NVK5_TIMING_WORDS) {
        if (why) *why = "length is not 13 u32 words";
        return false;
    }
    if (t[XEGFX_NVK5_T_TAG] != XEGFX_NVK5_TIMING_TAG) {
        if (why) *why = "bad NVK5 timing tag";
        return false;
    }

    const uint32_t pclk = t[XEGFX_NVK5_T_PCLK];
    const uint32_t hvis = t[XEGFX_NVK5_T_HVIS];
    const uint32_t hss  = t[XEGFX_NVK5_T_HSS];
    const uint32_t hse  = t[XEGFX_NVK5_T_HSE];
    const uint32_t htot = t[XEGFX_NVK5_T_HTOT];
    const uint32_t vvis = t[XEGFX_NVK5_T_VVIS];
    const uint32_t vss  = t[XEGFX_NVK5_T_VSS];
    const uint32_t vse  = t[XEGFX_NVK5_T_VSE];
    const uint32_t vtot = t[XEGFX_NVK5_T_VTOT];

    /* Same ordering rules as nvk5_timing_bad() upstream: the sync window has
     * to sit inside visible..total, and the blanking has to be non-zero. */
    if (!pclk || !hvis || !vvis) {
        if (why) *why = "zero clock or zero active size";
        return false;
    }
    if (!(hvis <= hss && hss < hse && hse <= htot)) {
        if (why) *why = "horizontal sync order";
        return false;
    }
    if (!(vvis <= vss && vss < vse && vse <= vtot)) {
        if (why) *why = "vertical sync order";
        return false;
    }
    if (htot == hvis || vtot == vvis) {
        if (why) *why = "zero blanking";
        return false;
    }
    if (hvis > 0xffffu || htot > 0xffffu || vvis > 0xffffu || vtot > 0xffffu) {
        if (why) *why = "field does not fit the 16-bit KAPI timing";
        return false;
    }

    const uint64_t totalPixels = (uint64_t)htot * (uint64_t)vtot;
    const uint32_t derivedMHz =
        (uint32_t)(((uint64_t)pclk * 1000ull + totalPixels / 2u) / totalPixels);
    uint32_t refresh = t[XEGFX_NVK5_T_REFRESH_MHZ];
    if (!refresh) {
        refresh = derivedMHz;
    } else if (refresh + 2u < derivedMHz || derivedMHz + 2u < refresh) {
        XLOG("mode: refresh word says %u mHz but pclk/(htot*vtot) is %u mHz; "
             "keeping the word", refresh, derivedMHz);
        refresh = derivedMHz;
    }

    bzero(out, sizeof *out);
    out->clockKHz       = pclk / 1000u;   /* exact Hz stays in *pixelClockHz */
    out->hdisplay       = (uint16_t)hvis;
    out->hsyncStart     = (uint16_t)hss;
    out->hsyncEnd       = (uint16_t)hse;
    out->htotal         = (uint16_t)htot;
    out->vdisplay       = (uint16_t)vvis;
    out->vsyncStart     = (uint16_t)vss;
    out->vsyncEnd       = (uint16_t)vse;
    out->vtotal         = (uint16_t)vtot;
    out->vscan          = 1;
    out->bpc            = 8;      /* panel is 8 bpc; the words do not carry it */
    out->pixelEncoding  = 0;      /* RGB */
    out->flags          = XEGFX_TF_PREFERRED;
    if (t[XEGFX_NVK5_T_HSYNCPOS])
        out->flags |= XEGFX_TF_HSYNC_POS;
    if (t[XEGFX_NVK5_T_VSYNCPOS])
        out->flags |= XEGFX_TF_VSYNC_POS;

    *pixelClockHz = pclk;
    *refreshMHz   = refresh;
    return true;
}

bool
XeGfxFramebuffer::buildMeasuredModeTable(const void *bytes, uint32_t length)
{
    fMeasuredModeCount = 0;
    fMeasuredOk        = false;

    if (!bytes || length < XEGFX_NVK5_TIMING_WORDS * sizeof(uint32_t)) {
        XLOG("mode: measured-panel-timing absent or short (%u B, need %u)",
             length,
             (uint32_t)(XEGFX_NVK5_TIMING_WORDS * sizeof(uint32_t)));
        return false;
    }

    /* OSData is not guaranteed to be 4-byte aligned; copy the record first. */
    uint32_t words[XEGFX_NVK5_TIMING_WORDS];
    memcpy(words, bytes, sizeof words);

    struct XeGfxKmsTiming t;
    uint64_t pclkHz     = 0;
    uint32_t refreshMHz = 0;
    const char *why     = "";
    if (!parseNvk5Timing(words, XEGFX_NVK5_TIMING_WORDS, &t, &pclkHz,
                         &refreshMHz, &why)) {
        XLOG("mode: measured-panel-timing rejected: %s", why);
        return false;
    }

    XLOG("mode: timing tag 0x%08x pclk %llu Hz raster %ux%u total %ux%u "
         "hsync %u..%u vsync %u..%u refresh %u.%03u Hz",
         words[XEGFX_NVK5_T_TAG], (unsigned long long)pclkHz,
         t.hdisplay, t.vdisplay, t.htotal, t.vtotal,
         t.hsyncStart, t.hsyncEnd, t.vsyncStart, t.vsyncEnd,
         refreshMHz / 1000u, refreshMHz % 1000u);

    /* ---- mode 1: native raster straight from the measured words ------ */
    struct XeGfxMeasuredMode *m = &fMeasuredModes[fMeasuredModeCount];
    bzero(m, sizeof *m);
    m->modeId       = 1;
    m->t            = t;
    m->pixelClockHz = pclkHz;
    m->refreshMHz   = refreshMHz;
    m->scaled       = false;

    timingToIo(&t, &m->dt);
    /* timingToIo works in kHz; put the exact measured Hz back. */
    m->dt.pixelClock = m->dt.minPixelClock = m->dt.maxPixelClock = pclkHz;
    /* The EDID detailed timing flags are 0x1a (H positive, V negative) and the
     * words agree (w11 = 1, w12 = 0); timingToIo already mapped that. */

    m->info.nominalWidth  = t.hdisplay;
    m->info.nominalHeight = t.vdisplay;
    /* 16.16 fixed point.  Widen before shifting: mHz<<16 overflows 32 bits
     * for anything above 65.535 Hz, which a 240 Hz panel would hit. */
    m->info.refreshRate   = (IOFixed1616)(((uint64_t)refreshMHz << 16) / 1000u);
    m->info.maxDepthIndex = 0;
    m->info.flags         = kDisplayModeValidFlag;
    /* EDID base block: 34 x 22 cm.  The 13 words carry no physical size. */
    m->info.imageWidth    = 0;
    m->info.imageHeight   = 0;  /* Physical dimensions come from the attached display's EDID. */

    /* Long-word-per-pixel 32bpp RGB, same encoding getPixelInformation()
     * reports for a registered mode. */
    bzero(&m->px, sizeof m->px);
    m->px.activeWidth      = (UInt32)t.hdisplay;
    m->px.activeHeight     = (UInt32)t.vdisplay;
    m->px.bytesPerRow      = (UInt32)t.hdisplay * 4u;
    m->px.bytesPerPlane    = m->px.bytesPerRow * m->px.activeHeight;
    m->px.bitsPerPixel     = 32;
    m->px.pixelType        = kIORGBDirectPixels;
    m->px.componentCount   = 3;
    m->px.bitsPerComponent = 8;
    m->px.componentMasks[0] = 0x00FF0000;   /* R */
    m->px.componentMasks[1] = 0x0000FF00;   /* G */
    m->px.componentMasks[2] = 0x000000FF;   /* B */
    m->px.componentMasks[3] = 0xFF000000;   /* alpha bits, unused by 24bpp */
    strlcpy(m->px.pixelFormat, IO32BitDirectPixels, sizeof m->px.pixelFormat);
    fMeasuredModeCount = 1;

    /* ---- mode 2: exact 2x HiDPI candidate (candidate only) ----------- */
    if (fMeasuredModeCount < XEGFX_DISPLAY_MAX_MEASURED_MODES &&
        !(t.hdisplay & 1u) && !(t.vdisplay & 1u)) {
        m = &fMeasuredModes[fMeasuredModeCount];
        *m = fMeasuredModes[0];          /* same raster and clock as native */
        m->modeId = 2;
        m->scaled = true;
        m->info.nominalWidth  = t.hdisplay / 2u;
        m->info.nominalHeight = t.vdisplay / 2u;
        /* Same convention as getTimingInfoForDisplayMode(): a scaled mode
         * reports the nominal (pre-scale) size and kIOScaleStretchToFit. */
        m->dt.horizontalScaled = m->info.nominalWidth;
        m->dt.verticalScaled   = m->info.nominalHeight;
        m->dt.scalerFlags      = kIOScaleStretchToFit;
        /* The backing store is nominal*2 == raster, so the aperture stays at
         * the full raster and the pipe can scan it out 1:1. */
        m->px.activeWidth  = (UInt32)t.hdisplay;
        m->px.activeHeight = (UInt32)t.vdisplay;
        fMeasuredModeCount++;
    }

    fMeasuredOk = true;
    logMeasuredModes();
    return true;
}

/*
 * K3: the panel's other refresh rates, from the core's fixed modes (KAPI modeset extension).  Only modes with the
 * measured raster are offered; the one at the measured refresh is the way back to it.  300 Hz runs on i915's joiner
 * (pipes A+B, over the 1.3056 GHz single-pipe dot clock); J1 has the accel write both pipes' PLANE_SURF.
 */
void
XeGfxFramebuffer::buildK3Modes()
{
    fK3 = false;
    fK3Count = 0;
    fS3Cur = 0;
    if (!fKapi || !fMeasuredOk || fMeasuredModeCount == 0 ||
        !(fKapi->flags & XEGFX_KMS_KAPI_FLAG_MODESET) || !fKapi->setMode) {
        XLOG("K3: core has no modeset extension; the measured refresh only");
        return;
    }
    const struct XeGfxMeasuredMode base = fMeasuredModes[0];
    bool haveNative = false;
    uint32_t next = 2;      /* [1] is the inactive HiDPI candidate */

    /*
     * The core publishes the nub from intel_bios_init(), before its K2 boot commit; the fixed modes answer NOT_READY
     * until that commit is done (10 s later on 2026-10-30 11:15, which cost that boot 240 Hz and every scaled mode).
     * macOS reads the mode list once, so wait for the commit here: 250 ms polls, at most 30 s.
     */
    {
        struct XeGfxKmsPanelTiming probe;
        uint32_t rc = XEGFX_ERR_NOT_READY;
        uint32_t waits = 0;
        for (; waits < 120; waits++) {
            bzero(&probe, sizeof probe);
            rc = fKapi->getTiming(fCtx, XEGFX_KMS_FIXED_MODE_BASE, &probe);
            if (rc != XEGFX_ERR_NOT_READY)
                break;
            IOSleep(250);
        }
        XLOG("K3: core fixed modes %s after %u ms (rc 0x%x)", rc == XEGFX_OK ? "ready" : "not available", waits * 250u, rc);
    }

    for (uint32_t n = 0; n < XEGFX_KMS_MAX_FIXED_MODES; n++) {
        struct XeGfxKmsPanelTiming pt;
        bzero(&pt, sizeof pt);
        uint32_t rc = fKapi->getTiming(fCtx, XEGFX_KMS_FIXED_MODE_BASE + n, &pt);
        if (rc != XEGFX_OK || pt.tag != XEGFX_NVK5_TIMING_TAG) {
            if (n == 0)
                XLOG("K3: no fixed mode from the core (rc 0x%x); the measured refresh only", rc);
            break;
        }
        uint32_t words[XEGFX_NVK5_TIMING_WORDS];
        memcpy(words, &pt, sizeof words);
        struct XeGfxKmsTiming t;
        uint64_t pclk = 0;
        uint32_t mhz = 0;
        const char *why = "";
        if (!parseNvk5Timing(words, XEGFX_NVK5_TIMING_WORDS, &t, &pclk, &mhz, &why)) {
            XLOG("K3: fixed mode %u rejected: %s", n, why);
            continue;
        }
        if (t.hdisplay != base.t.hdisplay || t.vdisplay != base.t.vdisplay)
            continue;
        if (mhz + 1000u > base.refreshMHz && mhz < base.refreshMHz + 1000u) {
            fKmsIdx[0] = n;
            haveNative = true;
            XLOG("K3: fixed mode %u = the measured %u.%03u Hz", n, mhz / 1000u, mhz % 1000u);
            continue;
        }
        /* J1: a mode over one pipe's dot clock (300 Hz) is joined across pipes A+B by the core; the accel mirrors
         * PLANE_SURF to pipe B, so it is offered like any other refresh rate. */
        if (pclk > 1305600000ull)
            XLOG("K3: fixed mode %u (%u Hz) runs joined across two pipes", n, mhz / 1000u);
        if (next >= XEGFX_DISPLAY_MAX_MEASURED_MODES)
            break;
        struct XeGfxMeasuredMode *m = &fMeasuredModes[next];
        *m = base;
        m->modeId       = (IODisplayModeID)(next + 1);
        m->t            = t;
        m->pixelClockHz = pclk;
        m->refreshMHz   = mhz;
        m->scaled       = false;
        timingToIo(&t, &m->dt);
        m->dt.pixelClock = m->dt.minPixelClock = m->dt.maxPixelClock = pclk;
        m->info.refreshRate = (IOFixed1616)(((uint64_t)mhz << 16) / 1000u);
        fKmsIdx[next] = n;
        XLOG("K3: mode %u = fixed mode %u, %ux%u %u.%03u Hz clock %llu Hz", (unsigned)m->modeId, n, t.hdisplay,
             t.vdisplay, mhz / 1000u, mhz % 1000u, (unsigned long long)pclk);
        next++;
        fK3Count++;
    }
    if (!haveNative) {
        fK3Count = 0;
        XLOG("K3: the measured refresh is not among the core's fixed modes; not offering others");
        return;
    }
    if (next > fMeasuredModeCount)
        fMeasuredModeCount = next;
    /* A single 4K60 EDID mode still supports scaler/HiDPI; extra refresh rates are optional. */
    fK3 = fK3Count > 0 || (haveNative && fConn.type == XEGFX_CONN_DP);
    setProperty("XeGfxK3Modes", (unsigned long long)fK3Count, 32);
}

int
XeGfxFramebuffer::s3Index(IODisplayModeID mode) const
{
    if (fMeasuredModeCount == 0)
        return -1;
    if (mode == fMeasuredModes[0].modeId)
        return 0;
    for (uint32_t i = 0; fK3 && i < fK3Count; i++)
        if (fMeasuredModes[2 + i].modeId == mode)
            return (int)(2 + i);
    int d = dtIndex(mode);
    if (d >= 0)
        return XEGFX_A1_DT_INDEX + d;
    return -1;
}

int
XeGfxFramebuffer::dtIndex(IODisplayModeID mode) const
{
    for (uint32_t i = 0; fScalerOk && i < fNDT; i++)
        if ((IODisplayModeID)fDT[i].detailedTimingModeID == mode)
            return (int)i;
    return -1;
}

/*
 * A1: a scaled desktop this framebuffer can put on the plane: both axes, stretch-to-fit only, no inset or border,
 * within the scanout allocation and the core's plane limit, at most 2x down (as NVRMFB), and a width whose row pitch is
 * whole 64-byte units (i915's linear stride; the accel refuses a frame whose pitch is not the plane's).
 */
bool
XeGfxFramebuffer::scaledOk(const IODetailedTimingInformationV2 *dt, const char **why) const
{
    const uint32_t sw = dt->horizontalScaled, sh = dt->verticalScaled;
    const char *w = nullptr;
    if (!fScalerOk) w = "no scaler (core has no setPlane)";
    else if (!sw || !sh) w = "one scaled axis";
    else if (dt->horizontalScaledInset || dt->verticalScaledInset) w = "inset";
    else if (dt->scalerFlags & ~(UInt32)kIOScaleStretchToFit) w = "rotation/axis flags";
    else if (dt->horizontalBorderLeft | dt->horizontalBorderRight | dt->verticalBorderTop | dt->verticalBorderBottom) w = "borders";
    else if (sw < 640 || sh < 400) w = "desktop below 640x400";
    else if (sw > XEGFX_KMS_PLANE_MAX_W || sh > XEGFX_KMS_PLANE_MAX_H) w = "desktop above the plane limit";
    else if ((uint64_t)sw * 4u * sh > fScanoutCapBytes) w = "desktop larger than the scanout allocation";
    else if (sw > 2 * dt->horizontalActive || sh > 2 * dt->verticalActive) w = "downscale beyond 2x";
    else if (sw & 15u) w = "width not a multiple of 16 (64-byte pitch)";
    if (why) *why = w ? w : "";
    return w == nullptr;
}

/* A1: put a w x h desktop on the plane (KAPI setPlane), then describe the scanout as that desktop */
IOReturn
XeGfxFramebuffer::setDesktop(uint32_t w, uint32_t h)
{
    if (w == fDeskW && h == fDeskH)
        return kIOReturnSuccess;
    XeGfxKmsSetPlaneFn setPlane = (XeGfxKmsSetPlaneFn)(uintptr_t)fKapi->reserved[3];
    const uint32_t pitch = w * 4u;
    uint64_t t0 = nowUs();
    uint32_t rc = setPlane ? setPlane(fCtx, w, h, pitch) : XEGFX_ERR_UNSUPPORTED;
    XLOG("A1: desktop %ux%u -> %ux%u pitch %u: KAPI setPlane rc 0x%x in %llu us", fDeskW, fDeskH, w, h, pitch, rc,
         (unsigned long long)(nowUs() - t0));
    if (rc != XEGFX_OK)
        return kIOReturnError;
    fDeskW = w;
    fDeskH = h;
    fScanout[0].width  = w;
    fScanout[0].height = h;
    fScanout[0].pitch  = pitch;
    m3dPublishScanoutProps();
    setProperty("XeGfxDesktop", (unsigned long long)(((uint64_t)w << 32) | h), 64);
    return kIOReturnSuccess;
}

/* A1: what NVRMFB publishes for this panel (docs/31 §31.4), with the plane limit as the scaler's maximum */
void
XeGfxFramebuffer::publishScalerCaps()
{
    IODisplayScalerInformation si;
    bzero(&si, sizeof si);
    si.scalerFeatures = kIOScaleCanUpSamplePixels | kIOScaleCanDownSamplePixels;
    si.maxHorizontalPixels = XEGFX_KMS_PLANE_MAX_W;
    si.maxVerticalPixels = XEGFX_KMS_PLANE_MAX_H;
    OSData *d = OSData::withBytes(&si, sizeof si);
    if (d) { setProperty(kIOFBScalerInfoKey, d); d->release(); }
    IODisplayTimingRangeV1 r;
    bzero(&r, sizeof r);
    r.minPixelClock = 25000000ull; r.maxPixelClock = 1200000000ull; r.maxPixelError = 0;
    r.supportedSyncFlags = kIORangeSupportsSeparateSyncs;
    r.supportedSignalLevels = 0; r.supportedSignalConfigs = kIODigitalSignal;
    r.minFrameRate = 23; r.maxFrameRate = 500; r.minLineRate = 10000; r.maxLineRate = 1000000;
    r.maxHorizontalTotal = 16384; r.maxVerticalTotal = 16384;
    r.charSizeHorizontalActive = r.charSizeHorizontalBlanking = r.charSizeHorizontalSyncOffset = r.charSizeHorizontalSyncPulse = 1;
    r.charSizeVerticalActive = r.charSizeVerticalBlanking = r.charSizeVerticalSyncOffset = r.charSizeVerticalSyncPulse = 1;
    r.charSizeHorizontalBorderLeft = r.charSizeHorizontalBorderRight = r.charSizeVerticalBorderTop = r.charSizeVerticalBorderBottom = 1;
    r.charSizeHorizontalTotal = r.charSizeVerticalTotal = 1;
    r.minHorizontalActiveClocks = 320;  r.maxHorizontalActiveClocks = 8192;
    r.minHorizontalBlankingClocks = 0;  r.maxHorizontalBlankingClocks = 8192;
    r.minHorizontalSyncOffsetClocks = 0; r.maxHorizontalSyncOffsetClocks = 8192;
    r.minHorizontalPulseWidthClocks = 1; r.maxHorizontalPulseWidthClocks = 8192;
    r.minVerticalActiveClocks = 200;    r.maxVerticalActiveClocks = 8192;
    r.minVerticalBlankingClocks = 0;    r.maxVerticalBlankingClocks = 8192;
    r.minVerticalSyncOffsetClocks = 0;  r.maxVerticalSyncOffsetClocks = 8192;
    r.minVerticalPulseWidthClocks = 1;  r.maxVerticalPulseWidthClocks = 8192;
    r.maxNumLinks = 1; r.minLink0PixelClock = 25000; r.maxLink0PixelClock = 1200000;
    r.supportedPixelEncoding = kIORangePixelEncodingRGB444; r.supportedBitsPerColorComponent = kIORangeBitsPerColorComponent8;
    r.supportedColorimetryModes = kIORangeColorimetryNativeRGB | kIORangeColorimetrysRGB;
    r.supportedDynamicRangeModes = kIORangeDynamicRangeSDR;
    OSData *g = OSData::withBytes(&r, sizeof r);
    if (g) { setProperty(kIOFBTimingRangeKey, g); g->release(); }
    XLOG("A1: published IOFBScalerInfo (up+down, max %ux%u) + IOFBTimingRange", XEGFX_KMS_PLANE_MAX_W,
         XEGFX_KMS_PLANE_MAX_H);
}

void
XeGfxFramebuffer::logMeasuredModes() const
{
    for (uint32_t i = 0; i < fMeasuredModeCount; i++) {
        const struct XeGfxMeasuredMode *m = &fMeasuredModes[i];
        const IODetailedTimingInformationV2 *dt = &m->dt;
        XLOG("mode: %u id %u: %ux%u nominal %ux%u@%u.%03u clk %llu Hz "
             "htot %u vtot %u hsync %u+%u vsync %u+%u polarity %c%c "
             "bpp %u%s",
             i, m->modeId,
             dt->horizontalActive, dt->verticalActive,
             m->info.nominalWidth, m->info.nominalHeight,
             m->refreshMHz / 1000u, m->refreshMHz % 1000u,
             (unsigned long long)dt->pixelClock,
             dt->horizontalActive + dt->horizontalBlanking,
             dt->verticalActive + dt->verticalBlanking,
             dt->horizontalSyncOffset, dt->horizontalSyncPulseWidth,
             dt->verticalSyncOffset, dt->verticalSyncPulseWidth,
             (dt->horizontalSyncConfig & kIOSyncPositivePolarity) ? '+' : '-',
             (dt->verticalSyncConfig & kIOSyncPositivePolarity) ? '+' : '-',
             m->px.bitsPerPixel,
             m->scaled ? " (HiDPI candidate, not activated)" : " (native)");
    }
}

/* ======================================================================= */
/* Detailed timings: CoreDisplay re-validates every mode it is given       */

IOReturn
XeGfxFramebuffer::timingFromIo(const IODetailedTimingInformationV2 *dt,
                               struct XeGfxKmsTiming *out) const
{
    if (!dt || !out)
        return kIOReturnBadArgument;
    bzero(out, sizeof *out);

    out->clockKHz = (uint32_t)(dt->pixelClock / 1000ull);

    out->hdisplay   = (uint16_t)dt->horizontalActive;
    out->htotal     = (uint16_t)(dt->horizontalActive + dt->horizontalBlanking);
    out->hsyncStart = (uint16_t)(dt->horizontalActive + dt->horizontalSyncOffset);
    out->hsyncEnd   = (uint16_t)(out->hsyncStart + dt->horizontalSyncPulseWidth);

    out->vdisplay   = (uint16_t)dt->verticalActive;
    out->vtotal     = (uint16_t)(dt->verticalActive + dt->verticalBlanking);
    out->vsyncStart = (uint16_t)(dt->verticalActive + dt->verticalSyncOffset);
    out->vsyncEnd   = (uint16_t)(out->vsyncStart + dt->verticalSyncPulseWidth);
    out->vscan      = 1;

    if (dt->horizontalSyncConfig & kIOSyncPositivePolarity)
        out->flags |= XEGFX_TF_HSYNC_POS;
    if (dt->verticalSyncConfig & kIOSyncPositivePolarity)
        out->flags |= XEGFX_TF_VSYNC_POS;

    out->bpc            = (uint8_t)dt->bitsPerColorComponent;
    out->pixelEncoding  = (uint8_t)dt->pixelEncoding;
    out->colorimetry    = (uint8_t)dt->colorimetry;
    out->dynamicRange   = (uint8_t)dt->dynamicRange;
    out->dsc            = dt->dscCompressedBitsPerPixel ? 1 : 0;
    return kIOReturnSuccess;
}

IOReturn
XeGfxFramebuffer::validateDetailedTiming(void *description,
                                         IOByteCount descripSize)
{
    if (!description || descripSize < sizeof(IODetailedTimingInformationV2))
        return kIOReturnBadArgument;

    /*
     * M3c S3: accept exactly one timing -- the measured raster that
     * enableController() publishes -- and name every other one in the log.
     * There is no "close enough" path into the KAPI here: S3 never calls
     * setMode, so accepting a foreign timing would only mislead CoreDisplay.
     */
    if (fS3) {
        /* CoreDisplay passes either a bare V2 timing or a whole IOFBDisplayModeDescription (as NVRMFB handles) */
        IODetailedTimingInformationV2 *dt = descripSize == sizeof(IOFBDisplayModeDescription)
            ? &((IOFBDisplayModeDescription *)description)->timingInfo.detailedInfo.v2
            : (IODetailedTimingInformationV2 *)description;
        if (dt->horizontalScaled || dt->verticalScaled) {
            const char *why = "";
            bool ok = timingMatchesAccepted(dt) && scaledOk(dt, &why);
            if (!ok && !*why) why = "raster is not a base mode";
            if (fValidateRejects < 48 || ok)
                XLOG("A1: validate desktop %ux%u on %ux%u pclk %llu id 0x%x -> %s", (unsigned)dt->horizontalScaled,
                     (unsigned)dt->verticalScaled, (unsigned)dt->horizontalActive, (unsigned)dt->verticalActive,
                     (unsigned long long)dt->pixelClock, (unsigned)dt->detailedTimingModeID, ok ? "OK" : why);
            if (!ok) {
                fValidateRejects++;
                return kIOReturnUnsupportedMode;
            }
            if (descripSize == sizeof(IOFBDisplayModeDescription)) {
                IODisplayModeInformation *info = &((IOFBDisplayModeDescription *)description)->info;
                const uint64_t tot = (uint64_t)(dt->horizontalActive + dt->horizontalBlanking) *
                                     (dt->verticalActive + dt->verticalBlanking);
                info->maxDepthIndex = 0;
                info->nominalWidth  = dt->horizontalScaled;
                info->nominalHeight = dt->verticalScaled;
                info->refreshRate   = tot ? (UInt32)((dt->pixelClock << 16) / tot) : 0;
            }
            return kIOReturnSuccess;
        }
        /*
         * Accept it even before enableController(): CoreDisplay validates the
         * timing it is about to use BEFORE the handshake, and refusing the
         * measured raster there would make it pick nothing at all.  This only
         * says "the raster is legal"; it publishes nothing.
         */
        if (timingMatchesAccepted(dt))
            return kIOReturnSuccess;

        fValidateRejects++;
        XLOG("S3: validateDetailedTiming drop #%u: %ux%u clk %llu Hz "
             "(accepted: %ux%u clk %llu Hz) ready=%d",
             fValidateRejects,
             (unsigned)dt->horizontalActive, (unsigned)dt->verticalActive,
             (unsigned long long)dt->pixelClock,
             (unsigned)fAcceptedDt.horizontalActive,
             (unsigned)fAcceptedDt.verticalActive,
             (unsigned long long)fAcceptedDt.pixelClock,
             fFbModeReady ? 1 : 0);
        return kIOReturnUnsupported;
    }

    struct XeGfxKmsTiming t;
    if (timingFromIo((const IODetailedTimingInformationV2 *)description, &t)
            != kIOReturnSuccess)
        return kIOReturnBadArgument;

    /*
     * Accept anything that matches a mode the engine already validated.  The
     * placeholder XeGfxIntelFB answers kIOReturnUnsupported here, and that is
     * why it can never change mode: CoreDisplay re-validates the timing it
     * got from getTimingInfoForDisplayMode before it uses it.
     *
     * Deliberately NOT accepting arbitrary user timings: i915 has to compute
     * watermarks, link training and DSC for a real pixel clock, and an
     * arbitrary one has no valid state.
     */
    if (findModeByTiming((const IODetailedTimingInformationV2 *)description) >= 0)
        return kIOReturnSuccess;
    return kIOReturnUnsupported;
}

int
XeGfxFramebuffer::findModeByTiming(const IODetailedTimingInformationV2 *dt) const
{
    for (uint32_t i = 0; i < fConn.numModes; i++) {
        const struct XeGfxKmsTiming *t = &fConn.modes[i].t;
        if (t->hdisplay   != dt->horizontalActive) continue;
        if (t->vdisplay   != dt->verticalActive)   continue;
        if (t->htotal     != dt->horizontalActive + dt->horizontalBlanking) continue;
        if (t->vtotal     != dt->verticalActive + dt->verticalBlanking)     continue;
        if ((uint32_t)(dt->pixelClock / 1000ull) != t->clockKHz)            continue;
        return (int)i;
    }
    return -1;
}

IOReturn
XeGfxFramebuffer::setDetailedTimings(OSArray *array)
{
    /*
     * M3c S3: CoreDisplay may offer the firmware timing, an EDID timing, a
     * user timing or its own copy of ours.  Keep only the entry that matches
     * the accepted measured raster, re-publish that one (so /options-independent
     * state can never drift), drop and count the rest with their identity in
     * the log.  No KAPI call, no register, no scanout.
     */
    if (fS3) {
        uint32_t offered = array ? array->getCount() : 0;
        uint32_t kept = 0, bad = 0, dropped = 0, nDT = 0;
        OSArray *filtered = OSArray::withCapacity(1);

        for (uint32_t i = 0; i < offered; i++) {
            OSData *entry = array ? OSDynamicCast(OSData, array->getObject(i))
                                  : nullptr;
            if (!entry ||
                entry->getLength() < sizeof(IODetailedTimingInformationV2)) {
                bad++;
                XLOG("S3: setDetailedTimings drop entry %u: %s (%u B)",
                     i, entry ? "short" : "not OSData",
                     entry ? entry->getLength() : 0);
                continue;
            }
            const IODetailedTimingInformationV2 *dt =
                (const IODetailedTimingInformationV2 *)entry->getBytesNoCopy();
            if (dt->horizontalScaled || dt->verticalScaled) {
                const char *why = "";
                if (!timingMatchesAccepted(dt) || !scaledOk(dt, &why) || nDT >= XEGFX_A1_MAX_DT) {
                    dropped++;
                    continue;
                }
                int base = 0;
                for (uint32_t k = 0; fK3 && k < fK3Count; k++)
                    if (xgDtMatchesBase(fMeasuredModes[2 + k].dt, dt))
                        base = (int)(2 + k);
                fDT[nDT] = *dt;
                fDtBase[nDT] = (uint8_t)base;
                nDT++;
                if (filtered)
                    filtered->setObject(entry);
                continue;
            }
            if (!timingMatchesAccepted(dt)) {
                dropped++;
                XLOG("S3: setDetailedTimings drop entry %u: %ux%u clk %llu Hz",
                     i, (unsigned)dt->horizontalActive,
                     (unsigned)dt->verticalActive,
                     (unsigned long long)dt->pixelClock);
                continue;
            }
            if (filtered && kept < 1 + (fK3 ? fK3Count : 0))
                filtered->setObject(entry);
            kept++;
        }

        if (filtered) {
            if (kept > 0)
                setProperty(kIOFBDetailedTimingsKey, filtered);
            filtered->release();
        }
        fNDT = fScalerOk ? nDT : 0;
        setProperty("XeGfxScaledModes", (unsigned long long)fNDT, 32);
        for (uint32_t i = 0; fBootDeskPending && i < fNDT; i++)
            if (fDtBase[i] == fS3Cur && fDT[i].horizontalScaled == fDeskW && fDT[i].verticalScaled == fDeskH) {
                fCurrentId = (IODisplayModeID)fDT[i].detailedTimingModeID;
                fCurrentIdx = 0;
                fBootDeskPending = false;
                XLOG("K4: the boot desktop %ux%u on mode %u is detailed timing 0x%x -- reported as the current mode",
                     fDeskW, fDeskH, (unsigned)fMeasuredModes[fS3Cur].modeId, (unsigned)fCurrentId);
            }
        fDtRejects += dropped + bad;
        XLOG("S3: setDetailedTimings: %u offered, %u kept + %u scaled desktops, %u dropped, %u "
             "malformed (running total %u)", offered, kept, fNDT, dropped, bad, fDtRejects);
        return kIOReturnSuccess;
    }

    /*
     * CoreDisplay hands back the timings it decided to use.  NVRMFB records
     * and does NOT rebuild the mode table from them: modes added here never
     * reach WindowServer (measured 36 added, 0 shown).  We do the same.
     */
    if (array)
        XLOG("setDetailedTimings: %u entr%s (table already published)",
             array->getCount(), array->getCount() == 1 ? "y" : "ies");
    return kIOReturnSuccess;
}

/* ======================================================================= */
/* Pixel info / apertures                                                  */
/* ======================================================================= */

IOReturn
XeGfxFramebuffer::getPixelInformation(IODisplayModeID mode, IOIndex depth,
                                      IOPixelAperture aperture,
                                      IOPixelInformation *info)
{
    if (fS3) {
        if (depth != 0 || aperture != kIOFBSystemAperture || !info ||
            (!fFbModeReady && !(fM3d && fM3dChecks == XEGFX_M3D_CHECKS_ALL)) || s3Index(mode) < 0)
            return kIOReturnBadArgument;
        *info = fMeasuredModes[0].px;   /* every K3 mode has the same raster */
        /* A1: the mode's own desktop, whatever is on the plane now (pitch = width * 4, as setDesktop programs it) */
        int si = s3Index(mode);
        if (si >= XEGFX_A1_DT_INDEX) {
            info->activeWidth  = fDT[si - XEGFX_A1_DT_INDEX].horizontalScaled;
            info->activeHeight = fDT[si - XEGFX_A1_DT_INDEX].verticalScaled;
        }
        info->bytesPerRow   = info->activeWidth * 4u;
        info->bytesPerPlane = info->bytesPerRow * info->activeHeight;
        return kIOReturnSuccess;
    }

    int idx = findModeByModeId(mode);
    if (idx < 0 || !info || depth != 0 || aperture != kIOFBSystemAperture)
        return kIOReturnBadArgument;

    const struct XeGfxKmsMode *m = &fConn.modes[idx];
    uint32_t w = m->t.hdisplay, h = m->t.vdisplay;

    bzero(info, sizeof *info);
    /* M5: with a scaled mode the scanout buffer is the *backing* size. */
    info->activeWidth      = (m->flags & XEGFX_MODE_SCALED) ? m->nominalWidth * 2 : w;
    info->activeHeight     = (m->flags & XEGFX_MODE_SCALED) ? m->nominalHeight * 2 : h;
    info->bytesPerRow      = (fScanoutCount ? fScanout[0].pitch : w * 4);
    /* P0-1: bytesPerPlane is a scalar UInt32 in IOPixelInformation (the
     * header comment even says "Not used"), not the OSData array the draft
     * assumed.  Report the whole plane; 0 would also be accepted. */
    info->bytesPerPlane    = info->bytesPerRow * info->activeHeight;
    info->bitsPerPixel     = 32;
    info->pixelType        = kIORGBDirectPixels;
    info->componentMasks[0] = 0x00FF0000;
    info->componentMasks[1] = 0x0000FF00;
    info->componentMasks[2] = 0x000000FF;
    info->componentMasks[3] = 0xFF000000;
    info->bitsPerComponent  = 8;
    /* P0-2: pixelFormat is IOPixelEncoding == char[64]; kIO32ARGBPixelFormat
     * is a userland symbol and cannot be assigned.  IOFramebuffer compares
     * this string, so it must be the canonical IO32BitDirectPixels name. */
    strlcpy(info->pixelFormat, IO32BitDirectPixels, sizeof info->pixelFormat);
    info->flags             = 0;
    /* pixelFormatString stays empty: it is only a hint for the debugger. */
    return kIOReturnSuccess;
}

IODeviceMemory *
XeGfxFramebuffer::getApertureRange(IOPixelAperture aperture)
{
    if (fInert)
        return nullptr;

    if (fS3) {
        /*
         * M3d: the aperture is only served once enableController() has both
         * published readiness and proven the panel scans this buffer.  It is
         * the exact physical range the KAPI mapped (single 64 MiB window,
         * < 4 GiB), so WindowServer draws straight into the scanout: no copy,
         * no shadow buffer.
         */
        if (!fM3d || !fM3dReady || !m3dApertureReady() || fScanoutCount == 0)
            return nullptr;
        if (aperture != kIOFBSystemAperture) {
            XLOG("M3d: aperture %u requested; only kIOFBSystemAperture exists",
                 (unsigned)aperture);
            return nullptr;
        }
        IOPhysicalAddress phys = (IOPhysicalAddress)fScanout[0].phys;
        IOByteCount len = (IOByteCount)fScanout[0].pitch * fScanout[0].height;
        /*
         * A1: the whole allocation, not the desktop on the plane now.  IOFramebuffer publishes this length as
         * IOFBMemorySize and CoreDisplay drops every mode whose pixels do not fit it -- with the 2560x1600 view that
         * was 2880x1800 and up, i.e. every HiDPI desktop above native (2026-10-30 11:25).
         */
        if (fScalerOk && fScanoutCapBytes > len)
            len = (IOByteCount)fScanoutCapBytes;
        if (!phys || !len || phys > 0xffffffffull) {
            XLOG("M3d: aperture rejected (phys 0x%llx len %llu)",
                 (unsigned long long)phys, (unsigned long long)len);
            return nullptr;
        }
        IODeviceMemory *mem = IODeviceMemory::withRange(phys, len);
        if (!mem) {
            XLOG("M3d: IODeviceMemory::withRange(0x%llx, %llu) failed",
                 (unsigned long long)phys, (unsigned long long)len);
            return nullptr;
        }
        if (fApertureLen != len) {
            fAperturePhys = phys;
            fApertureLen  = len;
            setProperty("XeGfxAperturePhys", (unsigned long long)phys, 64);
            setProperty("XeGfxApertureLen", (unsigned long long)len, 32);
            XLOG("M3d: aperture served phys 0x%llx len %llu (%ux%u pitch %u)",
                 (unsigned long long)phys, (unsigned long long)len,
                 fScanout[0].width, fScanout[0].height, fScanout[0].pitch);
        }
        /* IOFramebuffer semantics: one reference per call, consumed by the
         * caller.  A fresh instance per call is the NVRMFB-proven shape. */
        return mem;
    }

    if (aperture != kIOFBSystemAperture)
        return nullptr;
    if (!fAperture && fScanoutCount > 0 && fScanout[0].phys) {
        IOByteCount len = (IOByteCount)fScanout[0].pitch * fScanout[0].height;
        fAperture = IODeviceMemory::withRange(
            (IOPhysicalAddress)fScanout[0].phys, len);
        if (fAperture) {
            fAperturePhys = (IOPhysicalAddress)fScanout[0].phys;
            fApertureLen  = len;
            XLOG("aperture: legacy path phys 0x%llx len %llu",
                 (unsigned long long)fAperturePhys, (unsigned long long)len);
        }
    }
    return fAperture;
}

IODeviceMemory *
XeGfxFramebuffer::getVRAMRange()
{
    /* IOFramebufferUserClient asks this override directly for kIOFBVRAMMemory.
     * Returning null produces kIOReturnBadArgument, with no aperture fallback.
     * Both mappings must describe the same scanout; getApertureRange retains
     * the readiness gates and returns one owned reference per call. */
    return getApertureRange(kIOFBSystemAperture);
}

/* ======================================================================= */
/* M3: enableController -- the first real modeset                          */
/* ======================================================================= */

IOReturn
XeGfxFramebuffer::enableController()
{
    if (fS3) {
        if (fM3d) {
            /* M3d: the only IOFramebuffer path that may hand out an aperture.
             * open() has already mapped the scanout and latched one present;
             * this function re-runs the checks and publishes the ready flags. */
            fM3dChecks = m3dRunChecks("enableController");
            setProperty("XeGfxM3dChecks", (unsigned long long)fM3dChecks, 32);
            if (fM3dChecks != XEGFX_M3D_CHECKS_ALL) {
                XLOG("M3d: enableController -> Unsupported (checks 0x%x != "
                     "0x%x, why=%s); current output kept",
                     fM3dChecks, XEGFX_M3D_CHECKS_ALL,
                     fM3dWhy[0] ? fM3dWhy : "-");
                return kIOReturnUnsupported;
            }
            if (!fM3dCandidate || !m3dApertureReady()) {
                XLOG("M3d: enableController -> Unsupported (no mapped scanout)");
                return kIOReturnUnsupported;
            }

            /* XeGfxFbReady is legal here: super::start() returned before any
             * client could call open().  The first present then programs
             * PLANE_SURF; only after that does the mode read "ready". */
            if (!fPresented && !m3dEnsurePresented()) {
                XLOG("M3d: enableController -> Unsupported (first present "
                     "failed rc 0x%x)", fLastPresentRc);
                return kIOReturnUnsupported;
            }

            if (!fFbModeReady) {
                fFbModeReady = true;
                setProperty("XeGfxFbModeReady", kOSBooleanTrue);
                publishS3Timing();
            }
            m3dPublishReady();
            fM3dReady = true;
            setProperty("XeGfxM3dReady", kOSBooleanTrue);

            XLOG("M3d: enableController READY -- scanout 0x%llx %ux%u pitch %u "
                 "ggtt 0x%x, present=1, 1 IOFBDetailedTimings "
                 "(%ux%u clk %llu Hz %u.%03u Hz), aperture live, "
                 "0 adapter register writes",
                 (unsigned long long)fScanout[0].phys, fScanout[0].width,
                 fScanout[0].height, fScanout[0].pitch, fScanoutGGTT,
                 (unsigned)fAcceptedDt.horizontalActive,
                 (unsigned)fAcceptedDt.verticalActive,
                 (unsigned long long)fAcceptedDt.pixelClock,
                 fMeasuredModes[0].refreshMHz / 1000u,
                 fMeasuredModes[0].refreshMHz % 1000u);
            m3dLogState("enableController");
            return kIOReturnSuccess;
        }

        /* ---- M3c S3 (expose=0): validation only, no aperture ---------- */
        uint32_t mask = s3RunChecks("enableController");
        if (mask != XEGFX_S3_CHECKS_ALL) {
            /*
             * docs/12 §4.2: return Unsupported, do NOT fake success.  The
             * WindowServer keeps the output it already has instead of being
             * told a mode it cannot scan out was accepted.
             */
            XLOG("S3: enableController -> kIOReturnUnsupported "
                 "(a:KAPI=%u b:timing=%u c:detail=%u); current output kept",
                 !!(mask & XEGFX_S3_CHECK_KAPI),
                 !!(mask & XEGFX_S3_CHECK_TIMING),
                 !!(mask & XEGFX_S3_CHECK_DT));
            return kIOReturnUnsupported;
        }
        if (!fFbModeReady) {
            fFbModeReady = true;
            setProperty("XeGfxFbModeReady", kOSBooleanTrue);
            publishS3Timing();
            XLOG("S3: enableController checks a/b/c pass: XeGfxFbModeReady=TRUE, "
                 "1 IOFBDetailedTimings (%ux%u clk %llu Hz %u.%03u Hz); "
                 "0 register writes, 0 scanout",
                 (unsigned)fAcceptedDt.horizontalActive,
                 (unsigned)fAcceptedDt.verticalActive,
                 (unsigned long long)fAcceptedDt.pixelClock,
                 fMeasuredModes[0].refreshMHz / 1000u,
                 fMeasuredModes[0].refreshMHz % 1000u);
        } else {
            XLOG("S3: enableController re-entered; claim already published, "
                 "still 0 register writes");
        }
        return kIOReturnSuccess;
    }

    /* ---- pre-M3c paths: read-only, no modeset, no aperture ------------- */
    if (fReadOnly || fSkeleton || fDisableMode || !fKapi) {
        if (!fLoggedM2b) {
            XLOG("enableController: read-only -- no modeset, firmware raster "
                 "kept");
            fLoggedM2b = true;
        }
        fModeSet = false;
        return kIOReturnSuccess;   /* fake success: WindowServer must not fail open */
    }

    XLOG("enableController: KAPI v1 has no setMode entry point; firmware "
         "raster kept, no aperture, 0 register writes");
    fModeSet = false;
    return kIOReturnSuccess;
}

IOReturn
XeGfxFramebuffer::setDisplayMode(IODisplayModeID mode, IOIndex depth)
{
    if (fS3) {
        int si = s3Index(mode);
        if (depth != 0 || !fFbModeReady || si < 0) {
            XLOG("S3: setDisplayMode(%u, depth %u) refused (ready=%d)",
                 mode, depth, fFbModeReady ? 1 : 0);
            return kIOReturnUnsupported;
        }
        /*
         * K3: another refresh rate is a full modeset in i915 (KAPI setMode, ~1 s).  We are between IOFramebuffer's
         * will/did mode-change notifications, so the accel does not flip meanwhile; i915 puts its placeholder back on
         * the plane and the next flip replaces it.  On failure the pipe keeps the mode it had.
         */
        /* A1: a scaled mode runs on its base raster; the desktop is the scaled size, else the raster */
        const bool scaled = si >= XEGFX_A1_DT_INDEX;
        const int base = scaled ? fDtBase[si - XEGFX_A1_DT_INDEX] : si;
        const uint32_t dw = scaled ? fDT[si - XEGFX_A1_DT_INDEX].horizontalScaled : fMeasuredModes[0].t.hdisplay;
        const uint32_t dh = scaled ? fDT[si - XEGFX_A1_DT_INDEX].verticalScaled : fMeasuredModes[0].t.vdisplay;
        bool changed = false;
        if (fK3 && (uint32_t)base != fS3Cur) {
            uint64_t t0 = nowUs();
            uint32_t rc = fKapi->setMode(fCtx, 0, fKmsIdx[base]);
            XLOG("K3: setDisplayMode(0x%x) -> KAPI setMode(%u) %u.%03u Hz rc 0x%x in %llu us", mode, fKmsIdx[base],
                 fMeasuredModes[base].refreshMHz / 1000u, fMeasuredModes[base].refreshMHz % 1000u, rc,
                 (unsigned long long)(nowUs() - t0));
            if (rc != XEGFX_OK)
                return kIOReturnError;
            fS3Cur = (uint32_t)base;
            if (!publishAgdcTiming())
                return kIOReturnError;
            setProperty("XeGfxK3RefreshMHz", (unsigned long long)fMeasuredModes[base].refreshMHz, 32);
            changed = true;
        }
        if (fScalerOk && (dw != fDeskW || dh != fDeskH)) {
            if (setDesktop(dw, dh) != kIOReturnSuccess)
                return kIOReturnError;
            changed = true;
        }
        fBootDeskPending = false;
        k4SaveMode(fMeasuredModes[base].refreshMHz, fDeskW, fDeskH);
        if (changed) {
            fCurrentId  = mode;
            fCurrentIdx = 0;
            return kIOReturnSuccess;
        }
        fCurrentId  = mode;
        fCurrentIdx = 0;
        XLOG("S3: setDisplayMode(%u) accepted; raster unchanged, 0 register "
             "writes", mode);
        return kIOReturnSuccess;
    }

    if (depth != 0)
        return kIOReturnBadArgument;
    int idx = findModeByModeId(mode);
    if (idx < 0)
        return kIOReturnBadArgument;

    /* TODO(M3): drop the fReadOnly guard once setMode exists in the KAPI. */
    fCurrentId  = mode;
    fCurrentIdx = (uint32_t)idx;
    return kIOReturnSuccess;
}

/*
 * K4: what the core's boot commit ran.  The pipe starts there, so the refresh and the desktop start there too: the
 * scanout describes the desktop on the plane (the accel refuses a frame whose pitch is not the plane's), and the base
 * mode is current until setDetailedTimings names the scaled desktop.  An older core, or a boot that fell back to the
 * panel's preferred mode, leaves everything as before.
 */
void
XeGfxFramebuffer::k4ApplyBootState()
{
    if (IORegistryEntry *o = IORegistryEntry::fromPath("/options", gIODTPlane)) {
        OSData *d = OSDynamicCast(OSData, o->getProperty(XEGFX_NVRAM_BOOT_MODE));
        if (d && d->getLength() == 16) {
            uint32_t v[4];
            memcpy(v, d->getBytesNoCopy(), sizeof v);
            if (v[0] == XEGFX_BOOT_MODE_MAGIC) { fBootSavedMHz = v[1]; fBootSavedW = v[2]; fBootSavedH = v[3]; }
        }
        o->release();
    }
    if (!fK3 || !fKapi || fMeasuredModeCount == 0)
        return;
    struct XeGfxKmsPanelTiming pt;
    bzero(&pt, sizeof pt);
    const uint32_t rc = fKapi->getTiming(fCtx, XEGFX_KMS_BOOT_STATE_INDEX, &pt);
    uint32_t w[XEGFX_NVK5_TIMING_WORDS];
    memcpy(w, &pt, sizeof w);
    if (rc != XEGFX_OK || w[0] != XEGFX_KMS_BOOT_STATE_TAG) {
        XLOG("K4: no boot state from the core (rc 0x%x); it booted the panel's preferred mode", rc);
        return;
    }
    const uint32_t n = w[1], dw = w[2], dh = w[3], pitch = w[4];
    int base = -1;
    for (uint32_t i = 0; i < 2 + fK3Count && i < fMeasuredModeCount; i++)
        if (i != 1 && fKmsIdx[i] == n) { base = (int)i; break; }
    if (base < 0) {
        XLOG("K4: the core booted fixed mode %u, which is not in this mode table", n);
        return;
    }
    const uint32_t rw = fMeasuredModes[0].t.hdisplay, rh = fMeasuredModes[0].t.vdisplay;
    fS3Cur = (uint32_t)base;
    fCurrentId = fMeasuredModes[base].modeId;
    fCurrentIdx = 0;
    setProperty("XeGfxK3RefreshMHz", (unsigned long long)fMeasuredModes[base].refreshMHz, 32);
    if (dw != rw || dh != rh) {
        if (fScalerOk && pitch == dw * 4u && (uint64_t)pitch * dh <= fScanoutCapBytes) {
            fDeskW = dw;
            fDeskH = dh;
            fScanout[0].width  = dw;
            fScanout[0].height = dh;
            fScanout[0].pitch  = pitch;
            m3dPublishScanoutProps();
            setProperty("XeGfxDesktop", (unsigned long long)(((uint64_t)dw << 32) | dh), 64);
            fBootDeskPending = true;
        } else {
            /* this framebuffer cannot describe that desktop: put the raster back on the plane */
            XeGfxKmsSetPlaneFn setPlane = (XeGfxKmsSetPlaneFn)(uintptr_t)fKapi->reserved[3];
            const uint32_t prc = setPlane ? setPlane(fCtx, rw, rh, rw * 4u) : XEGFX_ERR_UNSUPPORTED;
            XLOG("K4: boot desktop %ux%u pitch %u does not fit this scanout; plane back to %ux%u (rc 0x%x)",
                 dw, dh, pitch, rw, rh, prc);
        }
    }
    setProperty("XeGfxBootMode", w[6] ? "saved" : "preferred");
    XLOG("K4: core booted fixed mode %u (%u.%03u Hz, %s), desktop %ux%u, joined %u -> current mode %u%s", n,
         fMeasuredModes[base].refreshMHz / 1000u, fMeasuredModes[base].refreshMHz % 1000u, w[6] ? "saved" : "preferred",
         fDeskW, fDeskH, w[5], (unsigned)fCurrentId, fBootDeskPending ? ", scaled desktop pending" : "");
}

/* K4: the mode WindowServer set last, for the core's next boot commit; written only when it changes */
void
XeGfxFramebuffer::k4SaveMode(uint32_t mhz, uint32_t w, uint32_t h)
{
    if (mhz == 0 || (mhz == fBootSavedMHz && w == fBootSavedW && h == fBootSavedH))
        return;
    IORegistryEntry *o = IORegistryEntry::fromPath("/options", gIODTPlane);
    if (!o)
        return;
    const uint32_t v[4] = { XEGFX_BOOT_MODE_MAGIC, mhz, w, h };
    bool ok = false;
    if (OSData *d = OSData::withBytes(v, sizeof v)) {
        ok = o->setProperty(XEGFX_NVRAM_BOOT_MODE, d);
        d->release();
    }
    o->release();
    if (ok) { fBootSavedMHz = mhz; fBootSavedW = w; fBootSavedH = h; }
    XLOG("K4: boot mode %u.%03u Hz desktop %ux%u %s NVRAM %s", mhz / 1000u, mhz % 1000u, w, h, ok ? "saved to" : "NOT saved to",
         XEGFX_NVRAM_BOOT_MODE);
}

IOReturn
XeGfxFramebuffer::getCurrentDisplayMode(IODisplayModeID *mode, IOIndex *depth)
{
    if (mode)  *mode  = fCurrentId;
    if (depth) *depth = 0;
    return kIOReturnSuccess;
}

/* ======================================================================= */
/* M3d: last-vblank timestamp for CoreDisplay's frame pacing               */
/* ======================================================================= */

void
XeGfxFramebuffer::getVBLTime(AbsoluteTime *time, AbsoluteTime *delta)
{
    uint64_t ns = fVblNs;
    if (!ns)
        ns = nowUs() * 1000ull;

    uint64_t periodNs = 16666666ull;
    if (fM3d && fMeasuredModeCount > 0 &&
        fMeasuredModes[fS3Cur].refreshMHz >= 1000u)
        periodNs = 1000000000000ull / fMeasuredModes[fS3Cur].refreshMHz;
    else if (fRefreshHz)
        periodNs = 1000000000ull / fRefreshHz;

    if (time)
        nanoseconds_to_absolutetime(ns, time);
    if (delta)
        nanoseconds_to_absolutetime(periodNs, delta);
}

/* ======================================================================= */
/* vblank: hardware through drm_vblank_work, IOTimer as the fallback       */
/* ======================================================================= */

void
XeGfxFramebuffer::vblankDeliver(uint64_t count, uint32_t deltaUs)
{
    /* P0-3: IOFBInterruptProc is (OSObject *target, void *ref); the delta is
     * only meaningful to the KAPI source, not to IOGraphicsFamily. */
    (void)count;
    (void)deltaUs;
    if (fVblLock) IOLockLock(fVblLock);
    if (fVblProc && fVblTarget)
        fVblProc(fVblTarget, fVblRef);
    if (fVblLock) IOLockUnlock(fVblLock);
}

/* E3: XeGfxVblankCount is a diagnostic; publishing it was a registry write on every tick (240 a second) */
void
XeGfxFramebuffer::vblankPublishCount(uint64_t now)
{
    if (now - fVblPropUs < 1000000ull)
        return;
    fVblPropUs = now;
    setProperty("XeGfxVblankCount", (unsigned long long)fVblCount, 64);
}

void
XeGfxFramebuffer::vblankBridge(void *arg, uint64_t count, uint32_t deltaUs)
{
    XeGfxFramebuffer *self = (XeGfxFramebuffer *)arg;
    self->vblankDeliver(count, deltaUs);
}

void
XeGfxFramebuffer::vblankTimerAction(OSObject *owner, IOTimerEventSource *src)
{
    XeGfxFramebuffer *self = (XeGfxFramebuffer *)owner;
    uint64_t now = self->nowUs();
    uint32_t delta = (uint32_t)(now - self->fLastVblUs);
    self->fLastVblUs = now;

    uint64_t periodUs = 16666;
    if (self->fM3d && self->fMeasuredModeCount > 0 &&
        self->fMeasuredModes[self->fS3Cur].refreshMHz >= 1000u)
        periodUs = 1000000000ull / self->fMeasuredModes[self->fS3Cur].refreshMHz;
    else if (self->fRefreshHz)
        periodUs = 1000000ull / self->fRefreshHz;
    if (periodUs < 1000)
        periodUs = 1000;
    if (delta == 0)
        delta = (uint32_t)periodUs;

    /* M3d: the accel owns the only real frame counter (BAR0 0x70040).  It is
     * a dry, lock-free PlatformFunction read, so the timer is just a sampling
     * clock; the count is only advanced when the accel saw a new frame. */
    if (self->fM3d) {
        uint64_t hwCount = 0, hwNs = 0;
        if (self->m3dAccelVblank(&hwCount, &hwNs)) {
            self->fVblSource = 2;
            if (hwCount != self->fAccelVblCount) {
                self->fAccelVblCount = hwCount;
                self->fVblCount      = hwCount;
                self->fVblNs         = hwNs ? hwNs : now * 1000ull;
            }
        } else {
            self->fVblSource = 1;
            self->fVblCount++;
            self->fVblNs = now * 1000ull;
        }
        self->vblankPublishCount(now);
    } else if (!self->fVblHW) {
        self->fVblCount++;
        self->fVblNs = now * 1000ull;
    }

    self->vblankDeliver(self->fVblCount, delta);
    if (self->fVblTimer)
        self->fVblTimer->setTimeoutUS((uint32_t)periodUs);
}

void
XeGfxFramebuffer::startVblankFallback()
{
    uint64_t periodUs = 16666;
    if (fM3d && fMeasuredModeCount > 0 && fMeasuredModes[fS3Cur].refreshMHz >= 1000u)
        periodUs = 1000000000ull / fMeasuredModes[fS3Cur].refreshMHz;
    else if (fRefreshHz)
        periodUs = 1000000ull / fRefreshHz;
    if (periodUs < 1000)
        periodUs = 1000;

    if (!fVblTimer && fWorkLoop) {
        fVblTimer = IOTimerEventSource::timerEventSource(
            this, &XeGfxFramebuffer::vblankTimerAction);
        if (fVblTimer) {
            if (fWorkLoop->addEventSource(fVblTimer) != kIOReturnSuccess) {
                fVblTimer->release();
                fVblTimer = nullptr;
            }
        }
    }
    if (!fVblTimer) {
        XLOG("vblank: no timer available; getVBLTime() synthesises time only");
        return;
    }
    fLastVblUs = nowUs();
    fVblTimer->enable();
    fVblTimer->setTimeoutUS((uint32_t)periodUs);
}


void
XeGfxFramebuffer::stopVblankFallback()
{
    if (fVblTimer)
        fVblTimer->cancelTimeout();
}

/*
 * E3 (native-display docs/32-FRAME-RATE.md): the hardware vblank.  The thread sleeps in the core's waitVblank, which
 * i915's vblank interrupt wakes, and hands each vblank to IOFramebuffer at once (its VBL handler is made for interrupt
 * context) with i915's timestamp, so CoreDisplay's vsync is the panel's and not a timer's in an arbitrary phase.  When
 * the pipe is off -- a modeset, sleep -- or no vblank comes within 50 ms, it ticks synthetically at the mode's period,
 * as the timer did, so WindowServer is never left without vblanks.
 */
void
XeGfxFramebuffer::vblankThreadMain(void *arg, wait_result_t wr)
{
    (void)wr;
    XeGfxFramebuffer *self = (XeGfxFramebuffer *)arg;
    XeGfxKmsWaitVblankFn wait = (XeGfxKmsWaitVblankFn)(uintptr_t)self->fKapi->reserved[5];
    uint64_t last = 0, lastUs = self->nowUs();
    uint32_t logged = 0;

    while (self->fVblThreadRun) {
        uint64_t periodUs = 16666;
        if (self->fMeasuredModeCount > 0 && self->fMeasuredModes[self->fS3Cur].refreshMHz >= 1000u)
            periodUs = 1000000000ull / self->fMeasuredModes[self->fS3Cur].refreshMHz;
        if (!self->fVblThreadOn) {
            IOSleep((unsigned)(periodUs / 1000 + 1));
            continue;
        }
        uint64_t count = 0, ageNs = 0, absNs = 0;
        const uint64_t before = self->nowUs();
        const uint32_t rc = wait(self->fCtx, last, 50000, &count, &ageNs);
        uint64_t now = self->nowUs();
        if (rc != XEGFX_OK && now - before < periodUs) {
            /* the pipe is off (sleep, a modeset): BUSY comes back at once -- tick at the mode's period, never spin */
            IOSleep((unsigned)((periodUs - (now - before)) / 1000 + 1));
            now = self->nowUs();
        }
        absolutetime_to_nanoseconds(mach_absolute_time(), &absNs);
        if (rc == XEGFX_OK) {
            last = count;
            self->fVblHwWaits++;
            self->fVblSource = 3;
            self->fVblCount = count;
            self->fVblNs = absNs > ageNs ? absNs - ageNs : absNs;
        } else {
            if (self->fVblHwMisses++ < 4 || (++logged & 0x3ff) == 0)
                XLOG("E3: waitVblank 0x%x after %llu vblanks -- synthetic tick (%llu so far)", rc,
                     (unsigned long long)self->fVblHwWaits, (unsigned long long)self->fVblHwMisses);
            self->fVblCount++;
            self->fVblNs = absNs;
        }
        const uint32_t delta = (uint32_t)(now - lastUs);
        lastUs = now;
        self->vblankDeliver(self->fVblCount, delta ? delta : (uint32_t)periodUs);
        self->vblankPublishCount(now);
    }
    __atomic_store_n(&self->fVblThreadLive, false, __ATOMIC_RELEASE);
    (void)thread_terminate(current_thread());
}

bool
XeGfxFramebuffer::startVblankThread()
{
    if (!fKapi || !(fKapi->flags & XEGFX_KMS_KAPI_FLAG_VBLANK) || !fKapi->reserved[5] || !fVblLock)
        return false;
    fVblThreadOn = true;
    if (fVblThreadLive)
        return true;
    thread_t t = nullptr;
    fVblThreadRun = true;
    fVblThreadLive = true;
    if (kernel_thread_start(&XeGfxFramebuffer::vblankThreadMain, this, &t) != KERN_SUCCESS) {
        fVblThreadRun = false;
        fVblThreadLive = false;
        fVblThreadOn = false;
        XLOG("E3: no vblank thread -- timer");
        return false;
    }
    thread_deallocate(t);
    XLOG("E3: vblank from i915's interrupt (core waitVblank)");
    return true;
}

void
XeGfxFramebuffer::stopVblankThread()
{
    fVblThreadOn = false;
    if (!fVblThreadLive)
        return;
    fVblThreadRun = false;
    for (int i = 0; i < 200 && __atomic_load_n(&fVblThreadLive, __ATOMIC_ACQUIRE); i++)
        IOSleep(1);	/* a wait returns within 50 ms */
    if (fVblThreadLive)
        XLOG("E3: vblank thread still running after 200 ms");
}

IOReturn
XeGfxFramebuffer::registerForInterruptType(IOSelect type, IOFBInterruptProc proc,
                                           OSObject *target, void *ref,
                                           void **interruptRef)
{
    if (type != kIOFBVBLInterruptType)
        return kIOReturnUnsupported;
    if (!proc || !interruptRef)
        return kIOReturnBadArgument;
    if (fVblProc && (fVblProc != proc || fVblTarget != target || fVblRef != ref))
        return kIOReturnNoResources;

    if (!fVblLock)
        fVblLock = IOLockAlloc();
    if (fVblLock) IOLockLock(fVblLock);
    fVblProc      = proc;
    fVblTarget    = target;
    fVblRef       = ref;
    if (fVblLock) IOLockUnlock(fVblLock);
    *interruptRef = this;

    /* No hardware interrupt is requested from the core KAPI in M3d (v1 has no
     * vblank entry at all).  M3d samples the accel's frame counter on a timer;
     * the other modes keep the synthetic timer-only behaviour unchanged. */
    if (fM3d)
        m3dStartVblankSource();
    else
        startVblankFallback();

    XLOG("vblank: client registered (%s source)", fM3d ? "M3d accel/timer" : "timer");
    return kIOReturnSuccess;
}

IOReturn
XeGfxFramebuffer::unregisterInterrupt(void *interruptRef)
{
    (void)interruptRef;
    if (fM3d)
        m3dStopVblankSource();
    else
        stopVblankFallback();
    if (fVblLock) IOLockLock(fVblLock);
    fVblProc    = nullptr;
    fVblTarget  = nullptr;
    fVblRef     = nullptr;
    if (fVblLock) IOLockUnlock(fVblLock);
    fVblCookie  = nullptr;
    fVblHW      = false;
    return kIOReturnSuccess;
}

IOReturn
XeGfxFramebuffer::setInterruptState(void *interruptRef, UInt32 state)
{
    if (interruptRef != this)
        return kIOReturnBadArgument;
    if (!fVblProc)
        return kIOReturnNotOpen;
    if (fM3d) {
        if (state)
            m3dStartVblankSource();
        else
            m3dStopVblankSource();
    } else if (state) {
        startVblankFallback();
    } else {
        stopVblankFallback();
    }
    return kIOReturnSuccess;
}

/* ======================================================================= */
/* M3d: preflight, scanout mapping, accel handshake, vblank plumbing       */
/* ======================================================================= */

/*
 * Everything in this section is dead unless fM3d was armed by startS3(),
 * and startS3() only arms it when phase==2 && trial==1 && fail==0 (fS3),
 * xegfx-disp-expose==1 and the full preflight is green.  No function below
 * writes a display register: the only MMIO writer in the M3d path is the
 * accel kext, called through the xgFlipPresent PlatformFunction.
 */

static const OSSymbol *gXgSymFlipPresent = nullptr;
static const OSSymbol *gXgSymVblankCount = nullptr;

static const OSSymbol *
xgSymbol(const char *name, const OSSymbol **cache)
{
    if (*cache)
        return *cache;
    *cache = OSSymbol::withCStringNoCopy(name);
    return *cache;
}

/* Kernel text/kext image pointers live in the high half on x86_64; refuse
 * anything else before the first dereference (a stale nub property here is
 * a kernel panic, not a bad mode). */
static bool
xgKernelPointerOk(uint64_t ptr)
{
    return ptr != 0 && (ptr >> 48) == 0xffffull;
}

/*
 * Find the accel service that publishes XeGfxAccelPresent=true on itself
 * (m3d-accel contract, 2026-10-28).  Only wait=true callers may block: those
 * are open()/enableController() (thread context).  The vblank timer path
 * only ever uses the cached fAccelObj.
 */
static IOService *
xgAccelScanOnce()
{
    // The nub also advertises XeGfxAccelPresent, but cannot handle presents.
    // Only the paired accelerator class and bundle may be selected.
    OSDictionary *matching = IOService::serviceMatching("NMXAccel");
    if (!matching)
        return nullptr;

    IOService *found = nullptr;
    OSIterator *iter = IOService::getMatchingServices(matching);
    if (iter) {
        while (OSObject *obj = iter->getNextObject()) {
            IOService *svc = OSDynamicCast(IOService, obj);
            if (!svc)
                continue;
            OSObject *present = svc->getProperty("XeGfxAccelPresent");
            OSString *ident = OSDynamicCast(OSString, svc->getProperty("CFBundleIdentifier"));
            if (svc->metaCast("NMXAccel") && ident &&
                ident->isEqualTo("org.watchthelight.XeGfxAccel") &&
                present && present->isEqualTo(kOSBooleanTrue)) {
                found = svc;
                found->retain(); // Transfer ownership before releasing iterator.
                break;
            }
        }
        iter->release();
    }
    matching->release();
    return found;
}

/*
 * Discover the M3d accel by its published property.  The property may appear
 * after our first probe (accel starts later in the boot chain), so wait=true
 * polls for up to ~500 ms from thread context; wait=false does one scan only
 * (thread_call/timer callers must never sleep here).
 */
static IOService *
xgFindAccelService(bool wait)
{
    IOService *found = xgAccelScanOnce();
    if (found || !wait)
        return found;
    for (int i = 0; i < 10; i++) {
        IOSleep(50);
        found = xgAccelScanOnce();
        if (found)
            return found;
    }
    return nullptr;
}

bool
XeGfxFramebuffer::m3dGateEnabled() const
{
    if (!XEGFX_DISPLAY_M3D_ENABLE)
        return false;
    if (!fS3 || !fM3d)
        return false;
    if (fStarts >= XEGFX_DISPLAY_MAX_STARTS)
        return false;
    return true;
}

/*
 * Does the KAPI's getTiming() raster agree with the measured M3a raster?
 * This replaces the M3c check (b) that compared against fConn.modes[]: the
 * v1 KAPI has no mode table, it has one 13-word timing tuple.
 */
bool
XeGfxFramebuffer::m3dTimingAgrees(const struct XeGfxKmsTiming *measured,
                                  uint32_t refreshMHz) const
{
    if (!fKapiTimingReady || !measured || fMeasuredModeCount == 0)
        return false;
    const uint32_t *w = fKapiTimingWords;
    if (w[0] != XEGFX_NVK5_TIMING_TAG)
        return false;

    bool raster = measured->hdisplay   == w[XEGFX_NVK5_T_HVIS] &&
                  measured->hsyncStart == w[XEGFX_NVK5_T_HSS] &&
                  measured->hsyncEnd   == w[XEGFX_NVK5_T_HSE] &&
                  measured->htotal     == w[XEGFX_NVK5_T_HTOT] &&
                  measured->vdisplay   == w[XEGFX_NVK5_T_VVIS] &&
                  measured->vsyncStart == w[XEGFX_NVK5_T_VSS] &&
                  measured->vsyncEnd   == w[XEGFX_NVK5_T_VSE] &&
                  measured->vtotal     == w[XEGFX_NVK5_T_VTOT];

    bool hPos = (measured->flags & XEGFX_TF_HSYNC_POS) != 0;
    bool vPos = (measured->flags & XEGFX_TF_VSYNC_POS) != 0;
    bool sync = hPos == (w[XEGFX_NVK5_T_HSYNCPOS] != 0) &&
                vPos == (w[XEGFX_NVK5_T_VSYNCPOS] != 0);

    uint64_t measuredHz = fMeasuredModes[0].pixelClockHz;
    /* getTiming copies the same NVK5 tuple as measured-panel-timing:
     * word 1 is Hz, word 10 is milli-Hz.  Only the local DRM mode uses kHz. */
    uint64_t kapiHz     = w[XEGFX_NVK5_T_PCLK];
    uint64_t delta      = measuredHz > kapiHz ? measuredHz - kapiHz
                                              : kapiHz - measuredHz;
    bool clock = measuredHz && kapiHz && delta * 200ull <= measuredHz;

    uint32_t kr  = w[XEGFX_NVK5_T_REFRESH_MHZ];
    uint32_t dr  = refreshMHz > kr ? refreshMHz - kr : kr - refreshMHz;
    uint32_t tol = kr / 50u;
    if (tol < 1000u)
        tol = 1000u;
    bool refresh = dr <= tol;

    if (!(raster && sync && clock && refresh))
        XLOG("M3d: timing mismatch raster=%d sync=%d clock=%d refresh=%d "
             "measured=%llu Hz/%u mHz KAPI=%llu Hz/%u mHz",
             raster, sync, clock, refresh,
             (unsigned long long)measuredHz, refreshMHz,
             (unsigned long long)kapiHz, kr);
    return raster && sync && clock && refresh;
}

uint32_t
XeGfxFramebuffer::m3dRunChecks(const char *who)
{
    uint32_t mask = 0;
    fM3dWhy[0] = 0;

    if (fKapi && xegfxKmsFlagsKnown(fKapiFlags)) {
        mask |= XEGFX_M3D_CHECK_KAPI;
    } else {
        strlcpy(fM3dWhy, "kapi", sizeof fM3dWhy);
        XLOG("M3d %s: check KAPI FAILED (table %p flags 0x%x)",
             who, (const void *)fKapi, fKapiFlags);
    }

    if (fChildDevices > 0) {
        mask |= XEGFX_M3D_CHECK_CONTRACT;
    } else {
        strlcpy(fM3dWhy, "contract", sizeof fM3dWhy);
        XLOG("M3d %s: check CONTRACT FAILED (vbt-child-devices=%u)",
             who, fChildDevices);
    }

    if (fKapiTimingReady && fMeasuredModeCount > 0 &&
        fMeasuredModes[0].t.hdisplay && fMeasuredModes[0].t.vdisplay) {
        mask |= XEGFX_M3D_CHECK_MODE;
    } else {
        strlcpy(fM3dWhy, "mode", sizeof fM3dWhy);
        XLOG("M3d %s: check MODE FAILED (kapiTimingReady=%d measured=%u)",
             who, fKapiTimingReady ? 1 : 0, fMeasuredModeCount);
    }

    if ((mask & XEGFX_M3D_CHECK_MODE) &&
        m3dTimingAgrees(&fMeasuredModes[0].t, fMeasuredModes[0].refreshMHz)) {
        mask |= XEGFX_M3D_CHECK_TIMING;
    } else if (mask & XEGFX_M3D_CHECK_MODE) {
        strlcpy(fM3dWhy, "timing", sizeof fM3dWhy);
        XLOG("M3d %s: check TIMING FAILED (measured raster != KAPI getTiming)",
             who);
    }

    if (xegfxKmsHasScanout(fKapi)) {
        mask |= XEGFX_M3D_CHECK_SCANOUT;
    } else {
        strlcpy(fM3dWhy, "scanout-ext", sizeof fM3dWhy);
        XLOG("M3d %s: check SCANOUT FAILED (core has no M3d scanout extension)",
             who);
    }

    if ((mask & XEGFX_M3D_CHECK_TIMING) && s3BuildAcceptedTiming()) {
        mask |= XEGFX_M3D_CHECK_DT;
    } else if (mask & XEGFX_M3D_CHECK_TIMING) {
        strlcpy(fM3dWhy, "detail-timing", sizeof fM3dWhy);
        XLOG("M3d %s: check DT FAILED", who);
    }

    return mask;
}

bool
XeGfxFramebuffer::m3dFindAccel(bool wait)
{
    if (fAccelObj)
        return true;
    if (!m3dGateEnabled())
        return false;

    IOService *svc = xgFindAccelService(wait);
    if (!svc)
        return false;

    /* The match hands over one reference the caller owns; keep it for the
     * lifetime of the FB and release in stop()/free(). */
    fAccelObj = svc; // xgAccelScanOnce transferred one retained reference.
    setProperty("XeGfxAccelFound", kOSBooleanTrue);
    XLOG("M3d: accel service %s found (XeGfxAccelPresent=true)",
         svc->getName());
    return true;
}

void
XeGfxFramebuffer::m3dPublishScanoutProps()
{
    if (!fScanoutMapped || fScanoutCount == 0)
        return;
    const struct XeGfxKmsScanoutInfo *sc = &fScanout[0];
    setProperty("XeGfxScanoutReady",  kOSBooleanTrue);
    setProperty("XeGfxScanoutHandle", (unsigned long long)fScanoutHandle, 32);
    setProperty("XeGfxScanoutPhys",   (unsigned long long)sc->phys, 64);
    setProperty("XeGfxScanoutPitch",  (unsigned long long)sc->pitch, 32);
    setProperty("XeGfxScanoutWidth",  (unsigned long long)sc->width, 32);
    setProperty("XeGfxScanoutHeight", (unsigned long long)sc->height, 32);
    setProperty("XeGfxScanoutGGTT",   (unsigned long long)fScanoutGGTT, 32);
}

/*
 * Map the first real scanout through the core KAPI extension.  Thread
 * context only (alloc may take a mutex and touch GGTT PTEs).  No display
 * register is written: the plane still points at the firmware buffer until
 * the accel's xgFlipPresent succeeds.
 */
bool
XeGfxFramebuffer::m3dAllocScanout()
{
    if (!m3dGateEnabled() || !xegfxKmsHasScanout(fKapi)) {
        strlcpy(fM3dWhy, "no-scanout-ext", sizeof fM3dWhy);
        return false;
    }
    if (fScanoutMapped && fScanoutCount > 0)
        return true;

    if (!fMeasuredOk || fMeasuredModeCount == 0 ||
        !fMeasuredModes[0].dt.horizontalActive ||
        !fMeasuredModes[0].dt.verticalActive) {
        strlcpy(fM3dWhy, "no-raster", sizeof fM3dWhy);
        XLOG("M3d: allocScanout skipped: no measured raster");
        return false;
    }

    uint32_t w = fMeasuredModes[0].dt.horizontalActive;
    uint32_t h = fMeasuredModes[0].dt.verticalActive;
    if (w < 2 || h < 2 ||
        w > XEGFX_KMS_SCANOUT_MAX_WIDTH || h > XEGFX_KMS_SCANOUT_MAX_HEIGHT) {
        strlcpy(fM3dWhy, "raster-limits", sizeof fM3dWhy);
        XLOG("M3d: allocScanout refused: %ux%u outside KAPI limits (%ux%u)",
             w, h, XEGFX_KMS_SCANOUT_MAX_WIDTH, XEGFX_KMS_SCANOUT_MAX_HEIGHT);
        return false;
    }

    XeGfxKmsAllocScanoutFn alloc = xegfxKmsAllocScanout(fKapi);
    if (!alloc) {
        strlcpy(fM3dWhy, "no-alloc-fn", sizeof fM3dWhy);
        return false;
    }

    struct XeGfxKmsScanoutInfo desc;
    bzero(&desc, sizeof desc);

    /* This is called only by startS3, before registration readiness. Core
     * serializes its allocator internally. Holding the counter/cursor lock
     * over a sleepable KAPI allocation can also block the watchdog. */
    /* A1: one allocation for the largest scaled desktop; the plane shows a w x h view of it (pitch w*4) */
    const uint32_t rasterW = w, rasterH = h;
    if (fScalerOk) {
        w = XEGFX_KMS_PLANE_MAX_W;
        h = XEGFX_KMS_PLANE_MAX_H;
    }
    XLOG("M3d: allocScanout BEGIN %ux%u (core call, no FB lock held)", w, h);
    uint32_t rc = alloc(fCtx, w, h, 4, XEGFX_KMS_SCANOUT_FLAG_ZERO, &desc);
    XLOG("M3d: allocScanout END rc=0x%x", rc);
    if (rc != XEGFX_OK && fScalerOk) {
        /* A1: no contiguous room for the largest desktop -- keep the raster-sized scanout, offer no scaled modes */
        XLOG("A1: %ux%u scanout failed (0x%x); retrying %ux%u without scaled desktops", w, h, rc, rasterW, rasterH);
        fScalerOk = false;
        removeProperty(kIOFBScalerInfoKey);
        removeProperty(kIOFBTimingRangeKey);
        w = rasterW;
        h = rasterH;
        bzero(&desc, sizeof desc);
        rc = alloc(fCtx, w, h, 4, XEGFX_KMS_SCANOUT_FLAG_ZERO, &desc);
        XLOG("M3d: allocScanout END rc=0x%x", rc);
    }

    if (rc != XEGFX_OK) {
        strlcpy(fM3dWhy, "alloc-failed", sizeof fM3dWhy);
        XLOG("M3d: core allocScanout(%ux%u) returned 0x%x", w, h, rc);
        return false;
    }
    if (!desc.phys || !desc.kva || !desc.pitch || !desc.length) {
        strlcpy(fM3dWhy, "alloc-incomplete", sizeof fM3dWhy);
        XLOG("M3d: core allocScanout returned incomplete descriptor "
             "(phys 0x%llx kva %p pitch %u len 0x%llx)",
             (unsigned long long)desc.phys, desc.kva, desc.pitch,
             (unsigned long long)desc.length);
        return false;
    }
    if (desc.phys > 0xffffffffull) {
        strlcpy(fM3dWhy, "phys-over-4g", sizeof fM3dWhy);
        XLOG("M3d: scanout phys 0x%llx above the 4 GiB aperture limit",
             (unsigned long long)desc.phys);
        return false;
    }
    if (desc.phys & 0xfffull) {
        /* The aperture and the accel phys fallback both build 4 KiB mappings;
         * a misaligned base means we must never expose it to WindowServer. */
        strlcpy(fM3dWhy, "phys-misaligned", sizeof fM3dWhy);
        XLOG("M3d: scanout phys 0x%llx is not 4 KiB aligned",
             (unsigned long long)desc.phys);
        return false;
    }
    if (desc.pitch < w * 4u ||
        (uint64_t)desc.pitch * h > XEGFX_KMS_SCANOUT_MAX_BYTES) {
        strlcpy(fM3dWhy, "geometry", sizeof fM3dWhy);
        XLOG("M3d: scanout geometry rejected (pitch %u width %u height %u)",
             desc.pitch, w, h);
        return false;
    }

    /* Core ABI: desc.reserved carries the low 32 bits of the GGTT offset when
     * the scanout is GGTT-mapped (0 = not mapped -> accel uses the phys path).
     * The accel present path requires the GGTT value to be 256 B aligned; the
     * core maps whole 4 KiB pages so this normally holds, but never forward a
     * misaligned value to the accel -- fall back to phys instead. */
    uint32_t ggtt = desc.reserved;
    if (ggtt && (ggtt & 0xffu)) {
        XLOG("M3d: ignoring misaligned core GGTT 0x%x (accel needs 256 B), "
             "present will use the phys fallback", ggtt);
        ggtt = 0;
        desc.reserved = 0;
    }

    fScanoutCapBytes = (uint64_t)desc.pitch * h;
    if (fScalerOk) {
        desc.width  = rasterW;
        desc.height = rasterH;
        desc.pitch  = rasterW * 4u;
    }
    fDeskW = desc.width;
    fDeskH = desc.height;
    fScanout[0]      = desc;
    fScanoutCount    = 1;
    fActiveScanout   = 0;
    fScanoutHandle   = desc.surfaceId;
    fScanoutGGTT     = ggtt;
    fScanoutMapped   = true;
    m3dPublishScanoutProps();

    XLOG("M3d: scanout handle 0x%x phys 0x%llx len 0x%llx pitch %u %ux%u "
         "ggtt 0x%x (GGTT-mapped, plane untouched)",
         fScanoutHandle, (unsigned long long)desc.phys,
         (unsigned long long)desc.length, desc.pitch, desc.width, desc.height,
         fScanoutGGTT);
    return true;
}

bool
XeGfxFramebuffer::m3dApertureReady() const
{
    if (!fScanoutMapped || fScanoutCount == 0)
        return false;
    if (!fScanout[0].phys || !fScanout[0].pitch || !fScanout[0].height)
        return false;
    return true;
}

/*
 * Publish the two flags the accel gates on.  Legal only after
 * IOFramebuffer::start() has returned: publishing earlier is the
 * getAttributeExt panic of 2026-10-27 (docs/15 section 2).
 */
void
XeGfxFramebuffer::m3dPublishReady()
{
    if (!fS3)
        return;

    if (!fReady) {
        fReady = true;
        setProperty("XeGfxFbReady", kOSBooleanTrue);
        XLOG("M3d: XeGfxFbReady=TRUE (super::start() returned; accel may program PLANE_SURF now)");
    }

    if (!fNoAccel) {
        IOService *provider = getProvider();
        if (provider)
            provider->setProperty("XeGfxDisplayPipeReady", kOSBooleanTrue);
        IOService *accel = OSDynamicCast(IOService, fAccelObj);
        if (accel)
            accel->setProperty("XeGfxDisplayPipeReady", kOSBooleanTrue);
    }
}

IOReturn
XeGfxFramebuffer::m3dPresent(struct XeGfxPresent *req)
{
    if (!req ||
        (req->version != XEGFX_M3D_PRESENT_VERSION &&
         req->version != XEGFX_M3D_PRESENT_VERSION_LEGACY) ||
        req->size != XEGFX_M3D_PRESENT_BYTES)
        return kIOReturnBadArgument;  /* accepts v2 and legacy v1 layouts */
    if (!m3dGateEnabled() || !m3dApertureReady())
        return kIOReturnNotReady;

    IOService *accel = OSDynamicCast(IOService, fAccelObj);
    if (!accel)
        return kIOReturnNotReady;

    const OSSymbol *sym = xgSymbol(XEGFX_M3D_FN_FLIP_PRESENT,
                                   &gXgSymFlipPresent);
    if (!sym)
        return kIOReturnNoMemory;

    /* Synchronous, thread context only: the accel may wait for latch (up to
     * 25 ms) unless PRESENT_NO_WAIT is set.  Never call from a gate or from
     * the vblank timer. */
    IOReturn rc = accel->callPlatformFunction(sym, false, req, nullptr,
                                              nullptr, nullptr);
    return rc;
}

/*
 * Present the current scanout to the accel and wait for it to accept.  The
 * first call also publishes XeGfxFbReady, which is the accel's gate.
 */
bool
XeGfxFramebuffer::m3dEnsurePresented()
{
    if (fPresented)
        return true;
    if (!m3dGateEnabled() || !m3dApertureReady())
        return false;

    /* Find the accel first: XeGfxFbReady is its gate, so publishing it while
     * no accel can consume it would only invite a head registration for a
     * framebuffer that is not yet guaranteed to open. */
    if (!fAccelObj && !m3dFindAccel(false)) {
        fLastPresentRc = kIOReturnNotReady;
        fPresentFails++;
        setProperty("XeGfxPresentRetries", (unsigned long long)fPresentFails, 32);
        if (fPresentFails <= 4 || (fPresentFails % 16u) == 0)
            XLOG("M3d: present: accel not published XeGfxAccelPresent yet "
                 "(attempt %u)", fPresentFails);
        return false;
    }

    m3dPublishReady();

    struct XeGfxPresent req;
    bzero(&req, sizeof req);
    req.version   = XEGFX_M3D_PRESENT_VERSION;
    req.size      = (uint32_t)sizeof req;
    req.phys      = fScanout[0].phys;
    req.kva       = (uint64_t)(uintptr_t)fScanout[0].kva;
    req.ggtt      = fScanoutGGTT;
    req.pitch     = fScanout[0].pitch;
    req.width     = fScanout[0].width;
    req.height    = fScanout[0].height;
    req.format    = XEGFX_M3D_FORMAT_ARGB;
    req.head      = 4;
    req.regionW   = fScanout[0].width;
    req.regionH   = fScanout[0].height;
    req.flipCount = ++fFlipCount;
    req.flags     = XEGFX_M3D_PRESENT_WAIT_LATCH;

    fLastPresentRc = m3dPresent(&req);
    if (fLastPresentRc == kIOReturnSuccess) {
        fPresented = true;
        setProperty("XeGfxPresent", kOSBooleanTrue);
        setProperty("XeGfxFbPresented", kOSBooleanTrue);
        XLOG("M3d: first present accepted (phys 0x%llx ggtt 0x%x pitch %u "
             "%ux%u flip %llu); head 4 now scans our buffer",
             (unsigned long long)fScanout[0].phys, fScanoutGGTT,
             fScanout[0].pitch, fScanout[0].width, fScanout[0].height,
             (unsigned long long)fFlipCount);
        return true;
    }

    fPresentFails++;
    setProperty("XeGfxPresentRetries", (unsigned long long)fPresentFails, 32);
    if (fPresentFails <= 4 || (fPresentFails % 16u) == 0)
        XLOG("M3d: present rc 0x%x (attempt %u, accel=%s)",
             fLastPresentRc, fPresentFails, fAccelObj ? "yes" : "no");
    return false;
}

void
XeGfxFramebuffer::m3dStartPresentRetry()
{
    if (fPresentTimer || fPresentGaveUp || fPresented)
        return;
    fPresentTimer = thread_call_allocate(&XeGfxFramebuffer::m3dPresentRetryCall,
                                         this);
    if (!fPresentTimer) {
        XLOG("M3d: present retry thread_call unavailable");
        return;
    }
    uint64_t deadline = 0;
    clock_interval_to_deadline(500, kMillisecondScale, &deadline);
    thread_call_enter_delayed(fPresentTimer, deadline);
    XLOG("M3d: present retry armed (500 ms cadence, gives up after 20 attempts)");
}

void
XeGfxFramebuffer::m3dStopPresentRetry()
{
    if (!fPresentTimer)
        return;
    thread_call_cancel(fPresentTimer);
    bool freed = false;
    for (int i = 0; i < 100; i++) {
        if (thread_call_free(fPresentTimer)) {
            freed = true;
            break;
        }
        IOSleep(10);
        thread_call_cancel(fPresentTimer);
    }
    if (!freed)
        XLOG("M3d: present retry thread_call busy at stop; leaking it");
    fPresentTimer = nullptr;
}

void
XeGfxFramebuffer::m3dPresentRetryCall(thread_call_param_t p0,
                                      thread_call_param_t p1)
{
    (void)p1;
    XeGfxFramebuffer *self = (XeGfxFramebuffer *)p0;
    if (self)
        self->m3dPresentRetryFire();
}

void
XeGfxFramebuffer::m3dPresentRetryFire()
{
    if (fPresented || fPresentGaveUp)
        return;

    if (m3dEnsurePresented()) {
        XLOG("M3d: present retry succeeded; the panel now scans our scanout");
        return;
    }
    if (fPresentFails >= 20) {
        fPresentGaveUp = true;
        setProperty("XeGfxM3dPresentFailed", kOSBooleanTrue);
        XLOG("M3d: present FAILED after %u attempts (last rc 0x%x); the panel "
             "keeps the firmware frame, open() stays denied and the S3 "
             "watchdog will count this boot as failed. Manual pull: "
             "xegfx-disp-fail=1", fPresentFails, fLastPresentRc);
        return;
    }
    uint64_t deadline = 0;
    clock_interval_to_deadline(500, kMillisecondScale, &deadline);
    thread_call_enter_delayed(fPresentTimer, deadline);
}

bool
XeGfxFramebuffer::m3dAccelVblank(uint64_t *count, uint64_t *ns)
{
    if (!fM3d || !fAccelObj)
        return false;
    IOService *accel = OSDynamicCast(IOService, fAccelObj);
    if (!accel)
        return false;
    const OSSymbol *sym = xgSymbol(XEGFX_M3D_FN_VBLANK_COUNT,
                                   &gXgSymVblankCount);
    if (!sym)
        return false;

    uint64_t c = 0, n = 0;
    IOReturn rc = accel->callPlatformFunction(sym, false, &c, &n, nullptr,
                                              nullptr);
    if (rc != kIOReturnSuccess)
        return false;
    if (count)
        *count = c;
    if (ns)
        *ns = n;
    return true;
}

void
XeGfxFramebuffer::m3dStartVblankSource()
{
    if (!fM3d)
        return;
    if (!fAccelObj)
        (void)m3dFindAccel(false);
    if (startVblankThread()) {
        stopVblankFallback();
        fVblSource = 3u;
        return;
    }
    fVblSource = fAccelObj ? 2u : 1u;
    startVblankFallback();
    XLOG("M3d: vblank source armed (%s)", fAccelObj ? "accel frame counter"
                                                    : "timer fallback");
}

void
XeGfxFramebuffer::m3dStopVblankSource()
{
    if (!fM3d)
        return;
    fVblThreadOn = false;	/* E3: the thread idles; stop() ends it */
    stopVblankFallback();
    fVblSource = 0;
}

void
XeGfxFramebuffer::m3dLogState(const char *who) const
{
    XLOG("M3d %s: gate=%d armed=%d candidate=%d checks=0x%x scanout=%d "
         "(handle 0x%x phys 0x%llx %ux%u pitch %u ggtt 0x%x) presented=%d "
         "fails=%u accel=%d vbl src=%u count=%llu ready=%d why=%s",
         who, m3dGateEnabled() ? 1 : 0, fM3d ? 1 : 0, fM3dCandidate ? 1 : 0,
         fM3dChecks, fScanoutMapped ? 1 : 0, fScanoutHandle,
         (unsigned long long)(fScanoutCount ? fScanout[0].phys : 0),
         fScanoutCount ? fScanout[0].width : 0,
         fScanoutCount ? fScanout[0].height : 0,
         fScanoutCount ? fScanout[0].pitch : 0, fScanoutGGTT,
         fPresented ? 1 : 0, fPresentFails, fAccelObj ? 1 : 0,
         fVblSource, (unsigned long long)fVblCount, fM3dReady ? 1 : 0,
         fM3dWhy[0] ? fM3dWhy : "-");
}

/* ======================================================================= */
/* cursor                                                                  */
/* ======================================================================= */

bool
XeGfxFramebuffer::hwCursorOn()
{
    if (fSwCursor < 0) {
        uint32_t sw = 0;
        xgNvramU32("xegfx-disp-swcursor", &sw);
        fSwCursor = sw != 0;
    }
    return !fSwCursor && fKapi && fKapi->reserved[6] != 0 && fKapi->reserved[7] != 0;
}

IOReturn
XeGfxFramebuffer::setCursorImage(void *cursorImage)
{
    /*
     * C1 (native-display docs/32-FRAME-RATE.md §32.18): IOFramebuffer converts WindowServer's cursor to 32-bit ARGB
     * here (convertCursorImage, at most 192x192 desktop pixels: the core scales it onto a 128x128 plane, 2/3 for the
     * HiDPI desktop on the 2560x1600 raster) and the core puts it on i915's cursor plane.  A refused image (too large,
     * or IOFramebuffer's discovery calls) is drawn by WindowServer itself.  Without the core's cursor this only
     * records, as before.
     */
    IOLockLock(fCursorLock);
    fCurSurface = 0;                 /* 0 = "re-upload needed" */
    IOLockUnlock(fCursorLock);
    if (!hwCursorOn()) return kIOReturnSuccess;
    const uint32_t dim = 192;
    const size_t bytes = (size_t)dim * dim * 4;
    uint32_t *staging = (uint32_t *)IOMalloc(bytes);
    if (!staging) return kIOReturnNoMemory;
    bzero(staging, bytes);
    IOHardwareCursorDescriptor d; bzero(&d, sizeof d);
    d.majorVersion = kHardwareCursorDescriptorMajorVersion;
    d.minorVersion = kHardwareCursorDescriptorMinorVersion;
    d.width = dim; d.height = dim;
    d.bitDepth = 32;
    d.supportedSpecialEncodings = kTransparentEncodedPixel;
    IOHardwareCursorInfo ci; bzero(&ci, sizeof ci);
    ci.majorVersion = kHardwareCursorInfoMajorVersion;
    ci.minorVersion = kHardwareCursorInfoMinorVersion;
    ci.hardwareCursorData = (UInt8 *)staging;
    const bool converted = convertCursorImage(cursorImage, &d, &ci);
    uint32_t rc = XEGFX_ERR_UNSUPPORTED;
    if (converted && ci.cursorWidth && ci.cursorHeight) {
        XeGfxKmsCursorImageFn img = (XeGfxKmsCursorImageFn)(uintptr_t)fKapi->reserved[6];
        rc = img(fCtx, staging, ci.cursorWidth, ci.cursorHeight, ci.cursorWidth * 4u);
    }
    IOFree(staging, bytes);
    if (rc != XEGFX_OK) {
        if (++fCurRefused <= 6) XLOG("C1: cursor image %ux%u %s (rc 0x%x) -- WindowServer draws it", (unsigned)ci.cursorWidth,
                                     (unsigned)ci.cursorHeight, converted ? "refused by the core" : "not converted", rc);
        return kIOReturnUnsupported;
    }
    if (++fCurImages <= 6 || (fCurImages % 500) == 0)
        XLOG("C1: hardware cursor image %u: %ux%u hot %u,%u", fCurImages, (unsigned)ci.cursorWidth, (unsigned)ci.cursorHeight,
             (unsigned)ci.cursorHotSpotX, (unsigned)ci.cursorHotSpotY);
    return kIOReturnSuccess;
}

IOReturn
XeGfxFramebuffer::setCursorState(SInt32 x, SInt32 y, bool visible)
{
    /*
     * DO NOT do a cursor flip here.  WindowServer calls this for every mouse
     * event from its cursor queue while holding CoreDisplay's cursor mutex; a
     * cursor commit takes ~0.3 ms and stalls the compositor while the mouse
     * moves.  Record and let the next apply coalesce, exactly like NVRMFB.
     */
    IOLockLock(fCursorLock);
    fCurX = x;
    fCurY = y;
    fCurVisible = visible;
    IOLockUnlock(fCursorLock);
    // C1: the core only writes CUR_POS/CUR_CTL/CUR_BASE (no commit), cheap enough for every mouse event
    if (hwCursorOn()) {
        XeGfxKmsCursorMoveFn move = (XeGfxKmsCursorMoveFn)(uintptr_t)fKapi->reserved[7];
        if (move(fCtx, x, y, visible ? 1u : 0u) != XEGFX_OK) return kIOReturnNotReady;
    }
    return kIOReturnSuccess;
}

void
XeGfxFramebuffer::cursorApplyLocked()
{
    /* M3d: the core KAPI v1 has no cursor entry point.  The accel owns the
     * hardware cursor registers; this is record-only, not an error. */
    if (fM3d && !fCursorLogged) {
        fCursorLogged = true;
        XLOG("M3d: cursor state recorded only (accel owns the hardware cursor)");
    }
}

/* ======================================================================= */
/* gamma / power                                                           */
/* ======================================================================= */

IOReturn
XeGfxFramebuffer::setGammaTable(UInt32 channelCount, UInt32 dataCount,
                                UInt32 dataWidth, void *data)
{
    return setGammaTable(channelCount, dataCount, dataWidth, data, false);
}


IOReturn
XeGfxFramebuffer::setGammaTable(UInt32 channelCount, UInt32 dataCount,
                                UInt32 dataWidth, void *data, bool syncToVBL)
{
    (void)syncToVBL;
    if (!data || channelCount == 0 || dataCount == 0)
        return kIOReturnBadArgument;
    if (fM3d && !fGammaLogged) {
        fGammaLogged = true;
        XLOG("M3d: setGammaTable(%u ch, %u entries, %u bits) recorded only "
             "(KAPI v1 has no gamma entry; accel/firmware owns LUTs)",
             channelCount, dataCount, dataWidth);
    }
    return kIOReturnSuccess;
}

IOReturn
XeGfxFramebuffer::setPowerState(unsigned long ordinal, IOService *device)
{
    (void)device;
    if (fM3d && !fSysSlept) {
        fSysSlept = true;
        XLOG("M3d: power state %lu recorded (adapter never writes display MMIO)",
             ordinal);
    }
    return kIOReturnSuccess;
}

/* ======================================================================= */
/* attributes                                                              */
/* ======================================================================= */

IOReturn
XeGfxFramebuffer::getAttribute(IOSelect attribute, uintptr_t *value)
{
    /*
     * NVRMFB forwards almost everything to super::getAttribute and only
     * answers two selectors itself.  Those two are verified against
     * IOGraphicsTypes.h; inventing selectors here does not compile, and the
     * ones the base class already answers correctly must not be shadowed.
     */
    if (attribute == kIOWindowServerActiveAttribute) {
        if (value) *value = 1;
        return kIOReturnSuccess;
    }
    if (attribute == kIOHardwareCursorAttribute) {
        /*
         * P1-6: without a cursor plane, reporting 1 would make WindowServer skip its software cursor and leave the
         * pointer invisible.  C1: with the core's cursor (KAPI reserved[6]/[7]) it is 1.
         */
        if (value) *value = hwCursorOn() ? 1 : 0;
        return kIOReturnSuccess;
    }
    return IOFramebuffer::getAttribute(attribute, value);
}

IOReturn
XeGfxFramebuffer::setAttribute(IOSelect attribute, uintptr_t value)
{
    switch (attribute) {
    case kIOWindowServerActiveAttribute:
        if (value)
            setProperty("IOFBServerReady", kOSBooleanTrue);
        break;
    default:
        return IOFramebuffer::setAttribute(attribute, value);
    }
    return kIOReturnSuccess;
}

IOReturn
XeGfxFramebuffer::getAttributeForConnection(IOIndex connectIndex,
                                            IOSelect attribute, uintptr_t *value)
{
    if (connectIndex != 0)
        return kIOReturnBadArgument;

    /*
     * As NVRMFB answers: IOGraphics reads the return code, not the value, and IODisplay::readFramebufferEDID()
     * asks kConnectionSupportsHLDDCSense with value == NULL (a NULL value used to fail the whole EDID read with
     * kIOReturnBadArgument).  DDC is "supported" exactly when there is an EDID, and Apple sense is never offered
     * on a connection, so IODisplay takes the display's identity from the EDID instead of a generic built-in
     * sense code (vendor 'unkn' product 0x717).  getAppleSense() itself still answers for the internal panel.
     */
    switch (attribute) {
    case kConnectionFlags:
        if (value)
            *value = (fConn.caps & XEGFX_CONN_CAP_INTERNAL) ? kIOConnectionBuiltIn : 0;
        return kIOReturnSuccess;
    case kConnectionSupportsHLDDCSense:
        if (value)
            *value = 1;
        return hasDDCConnect(0) ? kIOReturnSuccess : kIOReturnUnsupported;
    case kConnectionSupportsLLDDCSense:
    case kConnectionSupportsAppleSense:
    case kConnectionDisplayParameterCount:
    case kConnectionDisplayParameters:
        return kIOReturnUnsupported;
    default:
        return IOFramebuffer::getAttributeForConnection(connectIndex, attribute,
                                                        value);
    }
}

IOReturn
XeGfxFramebuffer::setAttributeForConnection(IOIndex connectIndex,
                                            IOSelect attribute, uintptr_t value)
{
    if (connectIndex != 0)
        return kIOReturnBadArgument;

    switch (attribute) {
    case kConnectionEnable:
    case kConnectionPower:
        return kIOReturnSuccess;
    case kConnectionFlushParameters:
        /* WindowServer asks us to push pending cursor state. */
        IOLockLock(fGate);
        cursorApplyLocked();
        IOLockUnlock(fGate);
        return kIOReturnSuccess;
    default:
        /* M4 backlight goes here: there is no kConnectionBacklightLevel in
         * the kernel headers; the panel is driven through the display
         * parameters dictionary (kIODisplayParametersBrightnessKey) which
         * XeGfxAGDC owns.  See docs/03-PLAN.md M4. */
        return IOFramebuffer::setAttributeForConnection(connectIndex, attribute,
                                                        value);
    }
}

/*
 * B1: laptop panel brightness, as NVRMFB does it (docs/31 §31.4).  macOS hands the brightness keys and the slider to
 * an AppleBacklightDisplay, which IOGraphics creates for a built-in LCD connection when a service named "backlight"
 * exists below the framebuffer before it is first opened.  This handler is that service: IODisplay offers
 * "brightness" and "linear-brightness" through it, and a change goes to the core's KAPI backlight entry (i915's own
 * backlight) on a thread call, because IODisplay calls doIntegerSet() with the framebuffer lock held.
 */
static const SInt32 kXeGfxMinBacklight = 5;   /* never 0: some panels switch the backlight off completely */

class XeGfxBacklightHandler : public IODisplayParameterHandler {
    OSDeclareDefaultStructors(XeGfxBacklightHandler)
    XeGfxKmsBacklightFn fFn = nullptr;
    void *fCtx = nullptr;
    thread_call_t fApply = nullptr;
    volatile SInt32 fPending = -1;
    SInt32 fLevel = 100;
    static void applyFire(thread_call_param_t me, thread_call_param_t);
public:
    static XeGfxBacklightHandler *with(XeGfxKmsBacklightFn fn, void *ctx, uint32_t level);
    bool setDisplay(IODisplay *display) APPLE_KEXT_OVERRIDE;
    bool doIntegerSet(OSDictionary *params, const OSSymbol *paramName, UInt32 value) APPLE_KEXT_OVERRIDE;
    bool doDataSet(const OSSymbol *, OSData *) APPLE_KEXT_OVERRIDE { return false; }
    bool doUpdate() APPLE_KEXT_OVERRIDE { return true; }
    void free() APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(XeGfxBacklightHandler, IODisplayParameterHandler)

XeGfxBacklightHandler *
XeGfxBacklightHandler::with(XeGfxKmsBacklightFn fn, void *ctx, uint32_t level)
{
    XeGfxBacklightHandler *h = OSTypeAlloc(XeGfxBacklightHandler);
    if (!h || !h->init()) { OSSafeReleaseNULL(h); return nullptr; }
    h->fFn = fn;
    h->fCtx = ctx;
    h->fApply = thread_call_allocate(applyFire, h);
    h->fLevel = level <= 100 ? (SInt32)level : 100;
    if (!h->fApply) { h->release(); return nullptr; }
    return h;
}

void
XeGfxBacklightHandler::free()
{
    if (fApply) {
        thread_call_cancel(fApply);
        while (!thread_call_free(fApply)) { thread_call_cancel(fApply); IOSleep(1); }
        fApply = nullptr;
    }
    IODisplayParameterHandler::free();
}

/* the display may have no IODisplayParameters yet; IODisplay::setProperties() only accepts parameters listed there */
bool
XeGfxBacklightHandler::setDisplay(IODisplay *display)
{
    if (!display) return false;
    OSObject *old = display->copyProperty(gIODisplayParametersKey);
    OSDictionary *cur = OSDynamicCast(OSDictionary, old);
    OSDictionary *params = cur ? OSDictionary::withDictionary(cur) : OSDictionary::withCapacity(2);
    OSSafeReleaseNULL(old);
    if (!params) return false;
    IODisplay::addParameter(params, gIODisplayBrightnessKey, 0, 100);
    IODisplay::setParameter(params, gIODisplayBrightnessKey, fLevel);
    IODisplay::addParameter(params, gIODisplayLinearBrightnessKey, 0, 100);
    IODisplay::setParameter(params, gIODisplayLinearBrightnessKey, fLevel);
    display->setProperty(gIODisplayParametersKey, params);
    params->release();
    XLOG("B1: brightness parameters on %s, panel at %d%%", display->getName(), (int)fLevel);
    return true;
}

bool
XeGfxBacklightHandler::doIntegerSet(OSDictionary *, const OSSymbol *paramName, UInt32 value)
{
    if (paramName != gIODisplayBrightnessKey && paramName != gIODisplayLinearBrightnessKey) return false;
    fPending = value > 100 ? 100 : (SInt32)value;
    thread_call_enter(fApply);
    return true;
}

void
XeGfxBacklightHandler::applyFire(thread_call_param_t p, thread_call_param_t)
{
    XeGfxBacklightHandler *me = (XeGfxBacklightHandler *)p;
    SInt32 v;
    do { v = me->fPending; } while (!OSCompareAndSwap((UInt32)v, (UInt32)-1, (volatile UInt32 *)&me->fPending));
    if (v < 0) return;
    int32_t percent = v < kXeGfxMinBacklight ? kXeGfxMinBacklight : v;
    uint32_t now = 0;
    uint32_t rc = me->fFn(me->fCtx, percent, &now);
    if (rc == XEGFX_OK) me->fLevel = v;
    else XLOG("B1: setting %d%% failed (0x%x)", (int)percent, rc);
}

/* B1: before IOFramebuffer::start(), because IOFramebuffer::open looks for "backlight" (gIOFBHaveBacklight) */
void
XeGfxFramebuffer::publishBacklight()
{
    if (!fKapi || !(fConn.caps & XEGFX_CONN_CAP_INTERNAL) || !(fKapi->flags & XEGFX_KMS_KAPI_FLAG_BACKLIGHT) ||
        !fKapi->reserved[4]) {
        XLOG("B1: no backlight control (internal %d, core flag %d)", (fConn.caps & XEGFX_CONN_CAP_INTERNAL) ? 1 : 0,
             (fKapi && (fKapi->flags & XEGFX_KMS_KAPI_FLAG_BACKLIGHT)) ? 1 : 0);
        return;
    }
    XeGfxKmsBacklightFn fn = (XeGfxKmsBacklightFn)(uintptr_t)fKapi->reserved[4];
    uint32_t level = 0;
    uint32_t rc = fn(fCtx, -1, &level);
    if (rc != XEGFX_OK) {
        XLOG("B1: core has no panel backlight (0x%x) -- no brightness control", rc);
        return;
    }
    fBacklight = XeGfxBacklightHandler::with(fn, fCtx, level);
    if (!fBacklight) { XLOG("B1: backlight handler allocation failed"); return; }
    fBacklight->setName("backlight");
    if (!fBacklight->attach(this)) {
        OSSafeReleaseNULL(fBacklight);
        XLOG("B1: backlight handler attach failed");
        return;
    }
    fBacklight->registerService(kIOServiceSynchronous);
    setProperty("XeGfxBacklight", (unsigned long long)level, 32);
    XLOG("B1: panel backlight at %u%% -- brightness keys and slider through AppleBacklightDisplay", level);
}

IOReturn
XeGfxFramebuffer::getAppleSense(IOIndex connectIndex, UInt32 *senseType,
                                UInt32 *primary, UInt32 *extended,
                                UInt32 *displayType)
{
    /* Only the internal panel answers, exactly like NVRM: no Apple sense on
     * a DP/eDP link, but AGDC uses this to find the backlight-capable head. */
    if (connectIndex != 0 || !(fConn.caps & XEGFX_CONN_CAP_INTERNAL))
        return kIOReturnUnsupported;
    if (senseType)   *senseType   = 0;
    if (primary)     *primary     = 0;
    if (extended)    *extended    = 0;
    /* B1: an LCD when there is a backlight handler -- AppleBacklightDisplay::probe takes only an LCD connection
     * (kGenericLCD = 20 in IOMacOSVideo.h); without one, 0 as before */
    if (displayType) *displayType = fBacklight ? 20 : 0;
    return kIOReturnSuccess;
}

IOReturn
XeGfxFramebuffer::getDDCBlock(IOIndex connectIndex, UInt32 blockNumber,
                              IOSelect blockType, IOOptionBits options,
                              UInt8 *data, IOByteCount *length)
{
    /* blockNumber is 1-based, as IODisplay asks and NVRMFB answers */
    if (connectIndex != 0 || blockType != kIODDCBlockTypeEDID ||
        blockNumber == 0 || blockNumber * 128u > fConn.edidSize)
        return kIOReturnUnsupported;
    if (!data || !length || *length < 128)
        return kIOReturnBadArgument;

    memcpy(data, fConn.edid + (blockNumber - 1) * 128u, 128);
    *length = 128;
    return kIOReturnSuccess;
}

/*
 * The core reads the panel's EDID over its AUX channel before it publishes the nub ("xegfx-edid", whole 128-byte
 * blocks, checksums already verified), the same way NVKMS hands NVRMFB its EDID.  No EDID keeps the old behaviour:
 * hasDDCConnect() stays false.  The EDID's image size fills in a physical size the VBT does not carry.
 */
void
XeGfxFramebuffer::loadNubEdid(IOService *provider)
{
    OSData *e = OSDynamicCast(OSData, provider->getProperty("xegfx-edid"));
    unsigned n = e ? e->getLength() : 0;

    n -= n % 128u;
    if (n > sizeof fConn.edid)
        n = sizeof fConn.edid;
    if (n < 128) {
        XLOG("edid: none on the nub; DDC stays off");
        return;
    }
    memcpy(fConn.edid, e->getBytesNoCopy(), n);
    fConn.edidSize = (uint16_t)n;
    const uint8_t *b = fConn.edid;
    unsigned wmm = b[66] | ((b[68] & 0xf0u) << 4), hmm = b[67] | ((b[68] & 0x0fu) << 8);   /* first detailed timing */
    if (!wmm || !hmm) { wmm = b[21] * 10u; hmm = b[22] * 10u; }                             /* basic block, in cm */
    if (!fConn.panelWidthMm && wmm && hmm) {
        fConn.panelWidthMm  = (uint16_t)wmm;
        fConn.panelHeightMm = (uint16_t)hmm;
    }
    XLOG("edid: %u byte(s) from the panel, vendor %c%c%c product 0x%04x, %ux%u mm",
         n, '@' + ((b[8] >> 2) & 0x1f), '@' + (((b[8] & 3) << 3) | (b[9] >> 5)), '@' + (b[9] & 0x1f),
         (unsigned)(b[10] | (b[11] << 8)), fConn.panelWidthMm, fConn.panelHeightMm);
    setProperty("xegfx-edid-bytes", (unsigned long long)n, 32);
}

/* ======================================================================= */
/* The accel-facing API: flip / scanout / vram                              */
/* ======================================================================= */

IOReturn
XeGfxFramebuffer::callPlatformFunction(const OSSymbol *functionName,
                                       bool waitForFunction,
                                       void *param1, void *param2,
                                       void *param3, void *param4)
{
    if (functionName == nullptr)
        return kIOReturnBadArgument;

    /* ---- accel -> FB: 80-byte v2 scanout description ------------------ */
    if (functionName->isEqualTo(XEGFX_M3D_FN_SCANOUT_INFO)) {
        struct XeGfxScanoutInfo *out = (struct XeGfxScanoutInfo *)param1;
        if (!out || !xgKernelPointerOk((uint64_t)(uintptr_t)out))
            return kIOReturnBadArgument;
        if (out->version != XEGFX_M3D_SCANOUT_INFO_VERSION ||
            out->size != XEGFX_M3D_SCANOUT_INFO_BYTES) {
            XLOG("M3d: xgGetScanoutInfo caller speaks v%u/%u, we speak v2/80",
                 out->version, out->size);
            return kIOReturnBadArgument;
        }
        bzero(out, sizeof *out);
        out->version = XEGFX_M3D_SCANOUT_INFO_VERSION;
        out->size    = XEGFX_M3D_SCANOUT_INFO_BYTES;
        out->head    = 4;
        // Kernel import describes allocated backing only. Requiring the
        // first open/present here makes initial pipe setup wait on a stage
        // that needs that pipe. User VRAM mapping keeps its separate ready
        // gate, and permission to program the plane is still XeGfxFbReady.
        if (!fFbStarted || !fM3dCandidate || fScanoutCount == 0 || !m3dApertureReady())
            return kIOReturnNotReady;
        const struct XeGfxKmsScanoutInfo *sc = &fScanout[0];
        if (!sc->phys || !sc->kva || !sc->pitch || !sc->width || !sc->height)
            return kIOReturnNotReady;
        uint32_t flags = XEGFX_M3D_SCANOUT_READY | XEGFX_M3D_SCANOUT_FBOWNS;
        if (fScanoutGGTT)
            flags |= XEGFX_M3D_SCANOUT_ZEROCOPY | XEGFX_M3D_SCANOUT_GGTT;
        out->phys          = sc->phys;
        out->kva           = (uint64_t)(uintptr_t)sc->kva;
        out->pitch         = sc->pitch;
        out->width         = sc->width;
        out->height        = sc->height;
        out->format        = XEGFX_M3D_FORMAT_ARGB;
        out->flags         = flags;
        out->activeScanout = fActiveScanout;
        out->scanoutCount  = fScanoutCount;
        out->vblankCount   = fVblCount;
        out->vblankNs      = fVblNs;
        out->ggtt          = fScanoutGGTT;
        out->reserved      = 0;
        return kIOReturnSuccess;
    }

    /* ---- accel/test -> FB: zero-copy present (FB forwards to accel) --- */
    if (functionName->isEqualTo(XEGFX_M3D_FN_FLIP_PRESENT)) {
        struct XeGfxPresent *req = (struct XeGfxPresent *)param1;
        if (!req || !xgKernelPointerOk((uint64_t)(uintptr_t)req))
            return kIOReturnBadArgument;
        if (!fM3d)
            return kIOReturnUnsupported;
        IOReturn rc = m3dPresent(req);
        if (rc == kIOReturnSuccess && !fPresented) {
            fPresented = true;
            setProperty("XeGfxPresent", kOSBooleanTrue);
            setProperty("XeGfxFbPresented", kOSBooleanTrue);
        }
        return rc;
    }

    /* ---- legacy M2b/M3 probe selectors (contract unchanged) ---------- */
    if (functionName->isEqualTo("xgGetDisplayInfo")) {
        uint32_t *out = (uint32_t *)param1;
        if (!out)
            return kIOReturnBadArgument;
        if (!fKapi)
            return kIOReturnNotReady;
        out[0] = (uint32_t)fConn.type;
        out[1] = (uint32_t)fConn.panelWidthMm;
        out[2] = (uint32_t)fConn.panelHeightMm;
        out[3] = (uint32_t)fHead;
        return kIOReturnSuccess;
    }
    if (functionName->isEqualTo("xgDisplayPower")) {
        bool *on = (bool *)param1;
        if (!on)
            return kIOReturnBadArgument;
        return setPowerState(*on ? 2ul : 0ul, nullptr);
    }
    if (functionName->isEqualTo("xgFramebufferReady")) {
        bool *ready = (bool *)param1;
        if (!ready)
            return kIOReturnBadArgument;
        *ready = fReady;
        return kIOReturnSuccess;
    }
    if (functionName->isEqualTo("xgFlipToSurface")) {
        XLOG("M3d: legacy xgFlipToSurface selector retired; use xgFlipPresent");
        return kIOReturnUnsupported;
    }

    return IOFramebuffer::callPlatformFunction(functionName, waitForFunction,
                                              param1, param2, param3, param4);
}

/* ======================================================================= */
/* M2b: IOKit matching gate                                                */
/* ======================================================================= */

/*
 * M2c gate, second spelling: a nub that publishes the M2c data contract
 * ("xegfx-display-version") is itself the opt-in.  The nub is only created by
 * the core when someone ran the display bring-up with xegfx_disp_level >= 1
 * (an explicit, manual action), so the adapter can bind to it and read the
 * VBT/child/timing snapshot without needing an OpenCore boot-arg (which the
 * bootloader would overwrite on every restart anyway).
 */
static bool
xgNubContractPresent(IOService *provider)
{
    return provider && provider->getProperty("xegfx-display-version") != nullptr;
}

IOService *
XeGfxFramebuffer::probe(IOService *provider, SInt32 *score)
{
    /*
     * Default OFF.  This is the difference between "the kext is installed" and
     * "the kext takes the panel": IOKit matches the personality to the nub,
     * then probe() is the only place that decides.  Enabled by either a
     * literal "-xedisp=1" (or "xedisp=1") in boot-args, or by matching an M2c
     * nub that carries the read-only data contract.
     */
    bool bootArg  = xgBootArgEnabled(XEGFX_DISPLAY_BOOTARG_ENABLE, false);
    bool contract = xgNubContractPresent(provider);
    if (!bootArg && !contract) {
        XLOG("probe: xedisp not enabled (want -xedisp=1) and no nub contract; "
             "declining %s", provider ? provider->getName() : "(no provider)");
        return nullptr;
    }

    XLOG("probe: enabled (boot-arg=%d nub-contract=%d), claiming %s",
         bootArg ? 1 : 0, contract ? 1 : 0,
         provider ? provider->getName() : "(no provider)");

    /* IOProbeScore is 10000 in Info.plist; keep it that high if the kernel
     * passed a lower default.  Never lower a score set by matching. */
    if (score && *score < 10000)
        *score = 10000;
    return this;
}

/* ======================================================================= */
/* lifecycle                                                               */
/* ======================================================================= */

uint64_t
XeGfxFramebuffer::nowUs() const
{
    clock_sec_t sec; clock_usec_t usec;
    clock_get_calendar_microtime(&sec, &usec);
    return ((uint64_t)sec * 1000000ull) + usec;
}

bool
XeGfxFramebuffer::start(IOService *provider)
{
    XLOG("starting (provider %s)", provider ? provider->getName() : "(none)");

    /*
     * Switch 1: master gate, default OFF.  probe() normally rejects the match
     * before start() is reached; start() re-checks because a future client may
     * call it directly.  "-xedisp=0" and "xedisp=0" also land here.  An M2c
     * nub that carries the data contract is the second (manual) opt-in.
     */
    if (!xgBootArgEnabled(XEGFX_DISPLAY_BOOTARG_ENABLE, false) &&
        !xgNubContractPresent(provider)) {
        XLOG("xedisp not enabled (want -xedisp=1) and no nub contract; declining");
        return false;
    }
    fDisableMode = !xgBootArgEnabled(XEGFX_DISPLAY_BOOTARG_NOMODE, true);
    fNoAccel     = !xgBootArgEnabled(XEGFX_DISPLAY_BOOTARG_NOACCEL, true);

    /* ---- switch 2: crash-loop guard --------------------------------- */
    if (xgStartsExhausted()) {
        XLOG("taken over %d times without reaching WindowServer; leaving the "
             "firmware raster to the placeholder", XEGFX_DISPLAY_MAX_STARTS);
        return false;
    }

    if (!provider) {
        XLOG("no provider");
        return false;
    }

    fGate       = IOLockAlloc();
    fCursorLock = IOLockAlloc();
    if (!fGate || !fCursorLock) {
        XLOG("lock allocation failed");
        return false;
    }

    /*
     * M4a: start from a known state.  These counters feed the claim's
     * boot-chain poller through the registry, so they must not inherit
     * whatever the first caller sees in freshly allocated object memory.
     */
    fOpenCount  = 0;
    fOpenDenied = 0;
    fOpened     = false;
    setProperty("XeGfxFbRegistered", kOSBooleanFalse);
    setProperty("XeGfxFbOpened", kOSBooleanFalse);

    /*
     * M2b: mirror the facts today's core nub publishes.  This happens before
     * any KAPI decision, so the ioreg record exists even for the bare M1b nub
     * (which has neither fb-index nor a KAPI table).
     */
    uint32_t displayLevel = xgUIntProperty(provider, "xegfx-display-level", 0);
    uint32_t bdbBlocks    = xgUIntProperty(provider, "vbt-bdb-blocks", 0);
    uint32_t childDevices = xgUIntProperty(provider, "vbt-child-devices", 0);
    fChildDevices = childDevices;
    XLOG("nub: xegfx-display-level=%u vbt-bdb-blocks=%u vbt-child-devices=%u",
         displayLevel, bdbBlocks, childDevices);
    setProperty("xegfx-display-level", (unsigned long long)displayLevel, 32);
    setProperty("vbt-bdb-blocks",      (unsigned long long)bdbBlocks, 32);
    setProperty("vbt-child-devices",   (unsigned long long)childDevices, 32);

    /* M2c: raw VBT + child records + measured timing snapshot.  Log only; the
     * KMS state machine starts consuming these in M3. */
    uint32_t fbIndex     = xgUIntProperty(provider, "fb-index", 0xffffffffu);
    uint32_t childSize   = xgUIntProperty(provider, "vbt-child-size", 0);
    OSData *vbtData      = OSDynamicCast(OSData, provider->getProperty("vbt-data"));
    OSData *childData    = OSDynamicCast(OSData, provider->getProperty("vbt-children"));
    OSData *timingData   = OSDynamicCast(OSData, provider->getProperty("measured-panel-timing"));
    XLOG("nub: fb-index=%u vbt-data=%u vbt-children=%u child-size=%u timing=%u",
         fbIndex,
         vbtData ? vbtData->getLength() : 0,
         childData ? childData->getLength() : 0,
         childSize,
         timingData ? timingData->getLength() : 0);
    setProperty("xegfx-nub-fb-index", (unsigned long long)fbIndex, 32);
    if (vbtData)
        setProperty("xegfx-nub-vbt-bytes", (unsigned long long)vbtData->getLength(), 32);
    if (childData && childSize)
        setProperty("xegfx-nub-child-count",
                    (unsigned long long)(childData->getLength() / childSize), 32);
    if (timingData)
        setProperty("xegfx-nub-timing-bytes", (unsigned long long)timingData->getLength(), 32);

    /*
     * M3a: turn the 13 measured words into IOFramebuffer mode records.  This
     * is pure computation on a copy of the property: no IOFramebuffer is
     * registered below in the skeleton path, so nothing queries these modes
     * and nothing is programmed.
     */
    if (timingData)
        buildMeasuredModeTable(timingData->getBytesNoCopy(),
                               timingData->getLength());
    else
        XLOG("mode: no measured-panel-timing property; 0 modes");

    /*
     * M3c S2: register as a second IOFramebuffer that is completely inert.
     * CoreDisplay may enumerate it, but open() is denied, it reports zero
     * connections and zero modes, and getApertureRange() returns NULL, so
     * nothing can ever be scanned out through it.  This proves the takeover
     * spine without moving a single register.
     */
    uint32_t phase = 0, trial = 0, fail = 0, expose = 0;
    xgNvramU32(XEGFX_DISPLAY_NVRAM_PHASE,  &phase);
    xgNvramU32(XEGFX_DISPLAY_NVRAM_TRIAL,  &trial);
    xgNvramU32(XEGFX_DISPLAY_NVRAM_FAIL,   &fail);
    xgNvramU32(XEGFX_DISPLAY_NVRAM_EXPOSE, &expose);
    fPhase  = phase;
    fTrial  = trial;
    fExpose = expose;
    fS3Fail = (fail != 0);

    /* M3d: the crash-loop counter is part of the gate, not just the claim. */
    uint32_t starts = 0;
    xgNvramU32(XEGFX_DISPLAY_NVRAM_STARTS, &starts);
    fStarts = starts;
    setProperty("xegfx-disp-phase",  (unsigned long long)phase, 32);
    setProperty("xegfx-disp-trial",  (unsigned long long)trial, 32);
    setProperty("xegfx-disp-fail",   (unsigned long long)fail, 32);
    setProperty("xegfx-disp-expose", (unsigned long long)expose, 32);

#if XEGFX_DISPLAY_S3_ENABLE
    /*
     * S3 gate: phase==2 AND trial==1 AND fail==0, checked before anything
     * else in the takeover path.  Every other combination is side-effect
     * free: with the gate closed there is no preflight, no XeGfxFbModeReady,
     * no detailed-timing property and no NVRAM write.
     */
    if (phase == 2 && trial == 1 && !fS3Fail)
        return startS3(provider);
#endif

    /*
     * xegfx-disp-fail=1 is a manual pull: register no IOFramebuffer at all so
     * the FB count cannot change, leave XeGfxFBClaim declining and let
     * XeGfxIntelFB keep the panel (docs/12 §4.4).
     */
    if (phase >= 1 && fS3Fail) {
        fSkeleton = true;
        setProperty("XeGfxDisplayFail", kOSBooleanTrue);
        setProperty("XeGfxDisplayVersion", "0.5-pull");
        if (!IOService::start(provider))
            return false;
        XLOG("S3: xegfx-disp-fail=1 pull -- no IOFramebuffer registered, "
             "no mode published, IntelFB keeps the panel");
        return true;
    }

    if (phase >= 1) {
        if (!IOFramebuffer::start(provider))
            return false;
        fFbStarted = true;
        setProperty("XeGfxFbRegistered", kOSBooleanTrue);
        fInert     = true;
        setProperty("XeGfxDisplayInert", kOSBooleanTrue);
        setProperty("XeGfxDisplayVersion", "0.5-s2");
        XLOG("S2: inert IOFramebuffer registered (phase=%u trial=%u); "
             "open() denied, 0 connections, 0 modes, no aperture, no register writes",
             phase, trial);
        return true;
    }

    OSData *kapiData =
        OSDynamicCast(OSData, provider->getProperty("xegfx-kms-kapi"));
    if (!kapiData) {
        /*
         * ===================== M2b skeleton =============================
         * The M1b nub publishes only the VBT facts above; fb-index and the
         * KAPI table arrive with the M2c core build.  Do NOT register an
         * IOFramebuffer and do NOT call a single display KAPI: this service
         * exists so ioreg proves the personality matched, nothing more.
         *
         * TODO(M3): delete this branch once the nub carries
         * "xegfx-kms-kapi" + "xegfx-timing".  The code below it is the real
         * takeover path and must only run after the M2c handshake.
         */
        fSkeleton = true;
        setProperty("XeGfxDisplaySkeleton", kOSBooleanTrue);
        setProperty("xegfx-nub-mode-count",
                    (unsigned long long)fMeasuredModeCount, 32);
        XLOG("M2b skeleton: nub has no xegfx-kms-kapi yet (M2c); "
             "no IOFramebuffer registered, no display registers touched");
        XLOG("M3a: %u read-only mode(s) built from measured-panel-timing; "
             "none registered, no enableController, no modeset",
             fMeasuredModeCount);
        if (!IOService::start(provider))
            return false;
        setProperty("XeGfxDisplayVersion", "0.5-m3a");
        XLOG("ready (skeleton): index %u head %u -- read-only", fIndex, fHead);
        return true;
    }

    /* ---- provider identity + KAPI (M2c path, kept for the next step) -- */
    OSData *idx = OSDynamicCast(OSData, provider->getProperty("fb-index"));
    if (!idx || idx->getLength() < sizeof(uint32_t)) {
        XLOG("provider has no fb-index");
        return false;
    }
    fIndex = *(const uint32_t *)idx->getBytesNoCopy();

    if (!readKapi(provider))
        return false;
    if (!kapiInit())
        return false;
    loadNubEdid(provider);
    if (!buildModeTable()) {
        XLOG("empty mode table");
        return false;
    }

    /* ---- become a console device ----------------------------------- */
    /*
     * This is what makes WindowServer use us from its first launch instead of
     * starting unaccelerated and restarting later.  XeGfxIntelFB returns true
     * here for the same reason.
     */
    fIsConsole = (fConn.caps & XEGFX_CONN_CAP_INTERNAL) != 0 || fConn.type == XEGFX_CONN_DP;

    if (!IOFramebuffer::start(provider))
        return false;
    fFbStarted = true;
    setProperty("XeGfxFbRegistered", kOSBooleanTrue);

    /* ---- AGDC / IOAccelDisplayPipe pairing -------------------------- */
    /* XeGfxAGDC matches on the class name and reads these two, plus
     * "xegfx-timing" on the nub for the raw panel timing. */
    setProperty("IOFBDependentID", (unsigned long long)0x5845474600000001ull);
    setProperty("IOFBDependentIndex", (unsigned long long)fIndex);

    fWorkLoop = getWorkLoop();
    if (fWorkLoop && fWorkLoop->addEventSource(
            fVblTimer = IOTimerEventSource::timerEventSource(
                this, &XeGfxFramebuffer::vblankTimerAction))) {
        fVblTimer->enable();
    }

    fReady = true;
    setProperty("XeGfxDisplayVersion", "0.5-m3a");
    XLOG("ready: index %u head %u console %d", fIndex, fHead, fIsConsole);
    return true;
}

/* ======================================================================= */
/* M3c S3: same-mode claim -- validate only, never program a register       */
/* ======================================================================= */

/*
 * docs/12 §4.2 / §7 S3, the three checks:
 *   (a) the nub publishes a KAPI table this adapter understands;
 *   (b) the measured NVK5 raster equals the engine's current KAPI mode;
 *   (c) those 13 words turn into one IODetailedTimingInformationV2.
 * start() runs them early so the result is visible in ioreg before anyone
 * opens the framebuffer; enableController() re-runs them and is the only
 * caller that publishes XeGfxFbModeReady.
 */
uint32_t
XeGfxFramebuffer::s3RunChecks(const char *who)
{
    uint32_t mask = 0;

    if (fKapi) {
        mask |= XEGFX_S3_CHECK_KAPI;
    } else {
        XLOG("S3 %s: check (a) FAILED -- nub has no usable xegfx-kms-kapi",
             who);
    }

    /*
     * (b) measured raster == engine raster.  The v1 KAPI has no connector
     * mode table (M3b retired it); the engine side is getTiming(), one 13-word
     * NVK5 tuple, so the M3d comparison is measured-vs-KAPI-timing.
     */
    if ((mask & XEGFX_S3_CHECK_KAPI) && fMeasuredOk &&
        fMeasuredModeCount > 0 && fKapiTimingReady) {
        if (m3dTimingAgrees(&fMeasuredModes[0].t,
                            fMeasuredModes[0].refreshMHz)) {
            mask |= XEGFX_S3_CHECK_TIMING;
        } else {
            XLOG("S3 %s: check (b) FAILED -- measured %ux%u tot %ux%u vs "
                 "KAPI getTiming %ux%u tot %ux%u",
                 who, fMeasuredModes[0].t.hdisplay,
                 fMeasuredModes[0].t.vdisplay, fMeasuredModes[0].t.htotal,
                 fMeasuredModes[0].t.vtotal, fKapiTimingWords[2],
                 fKapiTimingWords[6], fKapiTimingWords[5],
                 fKapiTimingWords[9]);
        }
    } else {
        XLOG("S3 %s: check (b) FAILED -- no measured mode or no KAPI timing "
             "(measured=%u ok=%d timingReady=%d)", who, fMeasuredModeCount,
             fMeasuredOk ? 1 : 0, fKapiTimingReady ? 1 : 0);
    }

    if (mask & XEGFX_S3_CHECK_TIMING) {
        if (s3BuildAcceptedTiming())
            mask |= XEGFX_S3_CHECK_DT;
        else
            XLOG("S3 %s: check (c) FAILED -- cannot build IOFBDetailedTimings "
                 "from the 13 measured words", who);
    }

    return mask;
}

bool
XeGfxFramebuffer::s3BuildAcceptedTiming()
{
    if (!fMeasuredOk || fMeasuredModeCount == 0)
        return false;

    const struct XeGfxMeasuredMode *m  = &fMeasuredModes[0];
    const IODetailedTimingInformationV2 *dt = &m->dt;

    if (!dt->pixelClock || !dt->horizontalActive || !dt->verticalActive ||
        !dt->horizontalBlanking || !dt->verticalBlanking)
        return false;
    if ((uint64_t)dt->horizontalActive + dt->horizontalBlanking !=
        (uint64_t)m->t.htotal)
        return false;
    if ((uint64_t)dt->verticalActive + dt->verticalBlanking !=
        (uint64_t)m->t.vtotal)
        return false;

    /* V2 has no timing-id field: CoreDisplay assigns the mode ids itself and
     * matches the published entry by its raster.  Keep the struct verbatim. */
    fAcceptedDt = *dt;
    return true;
}

/*
 * Is this timing the one we published?  Raster and sync layout must be
 * identical; the clock gets the same 0.5 % tolerance as check (b), because
 * CoreDisplay may round it through kHz.  The V2 struct carries no mode id, so
 * there is nothing caller-specific to ignore.
 */
static bool xgDtMatches(const IODetailedTimingInformationV2 &a, const IODetailedTimingInformationV2 *dt);

bool
XeGfxFramebuffer::timingMatchesAccepted(const IODetailedTimingInformationV2 *dt) const
{
    if (!dt)
        return false;
    if (xgDtMatches(fAcceptedDt, dt))
        return true;
    for (uint32_t i = 0; fK3 && i < fK3Count; i++)
        if (xgDtMatches(fMeasuredModes[2 + i].dt, dt))
            return true;
    return false;
}

/* the accepted-timing comparison, against one reference: exact raster and sync, clock within 0.5% */
static bool
xgDtMatches(const IODetailedTimingInformationV2 &a, const IODetailedTimingInformationV2 *dt)
{
    if (!a.pixelClock || !a.horizontalActive || !a.verticalActive)
        return false;

    if (dt->horizontalActive      != a.horizontalActive ||
        dt->verticalActive        != a.verticalActive ||
        dt->horizontalBlanking    != a.horizontalBlanking ||
        dt->verticalBlanking      != a.verticalBlanking ||
        dt->horizontalSyncOffset  != a.horizontalSyncOffset ||
        dt->horizontalSyncPulseWidth != a.horizontalSyncPulseWidth ||
        dt->verticalSyncOffset    != a.verticalSyncOffset ||
        dt->verticalSyncPulseWidth != a.verticalSyncPulseWidth)
        return false;

    bool hPos = (dt->horizontalSyncConfig & kIOSyncPositivePolarity) != 0;
    bool vPos = (dt->verticalSyncConfig & kIOSyncPositivePolarity) != 0;
    bool aH   = (a.horizontalSyncConfig & kIOSyncPositivePolarity) != 0;
    bool aV   = (a.verticalSyncConfig & kIOSyncPositivePolarity) != 0;
    if (hPos != aH || vPos != aV)
        return false;

    uint64_t clock = dt->pixelClock;
    uint64_t delta = clock > a.pixelClock ? clock - a.pixelClock
                                          : a.pixelClock - clock;
    return delta * 200ull <= a.pixelClock;
}

bool
XeGfxFramebuffer::publishAgdcTiming()
{
    struct XeGfxKmsPanelTiming pt;
    bzero(&pt, sizeof pt);
    if (!fKapi || fKapi->getTiming(fCtx, fIndex, &pt) != XEGFX_OK) {
        removeProperty("NVRMTiming");
        XLOG("AGDC: current KAPI timing unavailable; capabilities remain unavailable");
        return false;
    }
    uint32_t words[XEGFX_NVK5_TIMING_WORDS];
    memcpy(words, &pt, sizeof words);
    struct XeGfxKmsTiming parsed;
    uint64_t clockHz = 0;
    uint32_t refreshMHz = 0;
    const char *why = "";
    if (!parseNvk5Timing(words, XEGFX_NVK5_TIMING_WORDS, &parsed,
                         &clockHz, &refreshMHz, &why)) {
        removeProperty("NVRMTiming");
        XLOG("AGDC: current KAPI timing rejected: %s", why);
        return false;
    }
    OSData *data = OSData::withBytes(words, sizeof words);
    const bool ok = data && setProperty("NVRMTiming", data);
    if (data)
        data->release();
    if (!ok)
        removeProperty("NVRMTiming");
    XLOG("AGDC: current KAPI timing %ux%u clock %llu Hz refresh %u mHz %s",
         (unsigned)parsed.hdisplay, (unsigned)parsed.vdisplay,
         (unsigned long long)clockHz, refreshMHz, ok ? "published" : "allocation failed");
    return ok;
}

void
XeGfxFramebuffer::publishS3Timing()
{
    OSData *data = OSData::withBytes(&fAcceptedDt, sizeof fAcceptedDt);
    if (!data) {
        XLOG("S3: OSData allocation failed; IOFBDetailedTimings NOT published");
        return;
    }
    OSArray *array = OSArray::withCapacity(1);
    if (!array) {
        data->release();
        XLOG("S3: OSArray allocation failed; IOFBDetailedTimings NOT published");
        return;
    }
    array->setObject(data);
    setProperty(kIOFBDetailedTimingsKey, array);
    XLOG("S3: published %s with 1 entry (%ux%u clk %llu Hz)",
         kIOFBDetailedTimingsKey,
         (unsigned)fAcceptedDt.horizontalActive,
         (unsigned)fAcceptedDt.verticalActive,
         (unsigned long long)fAcceptedDt.pixelClock);
    array->release();
    data->release();
}

void
XeGfxFramebuffer::logS3Gates(const char *who) const
{
    XLOG("S3 %s: gates phase=%u trial=%u expose=%u fail=%d "
         "(arming requires phase==2 && trial==1; expose is M3d-only)",
         who, fPhase, fTrial, fExpose, fS3Fail ? 1 : 0);
}

void
XeGfxFramebuffer::s3L0PublishArm()
{
    if (fL0Timer)
        return;
    fL0Timer = thread_call_allocate(&XeGfxFramebuffer::s3L0PublishCall, this);
    if (!fL0Timer) {
        XLOG("S3: L0 publish thread_call unavailable; L0Ready only via "
             "enableController on the M3d path");
        return;
    }
    uint64_t deadline = 0;
    clock_interval_to_deadline(3000, kMillisecondScale, &deadline);
    thread_call_enter_delayed(fL0Timer, deadline);
}

void
XeGfxFramebuffer::s3L0PublishCall(thread_call_param_t p0,
                                  thread_call_param_t p1)
{
    (void)p1;
    XeGfxFramebuffer *self = (XeGfxFramebuffer *)p0;
    if (self)
        self->s3L0PublishFire();
}

void
XeGfxFramebuffer::s3L0PublishFire()
{
    if (fL0Timer) {
        thread_call_free(fL0Timer);
        fL0Timer = nullptr;
    }
    if (!fS3 || fInert)
        return;
    if (fM3d && (!fM3dCandidate || !fWorkLoop)) {
        XLOG("S3: L0 gate remains closed (scanout or controller not ready)");
        return;
    }
    if (fS3Checks == XEGFX_S3_CHECKS_ALL) {
        setProperty("XeGfxL0Ready", kOSBooleanTrue);
        XLOG("S3: XeGfxL0Ready=TRUE (delayed 3 s past start() return; accel/"
             "AGDC may pair head 4 now, m3d=%d)", fM3dCandidate ? 1 : 0);
    }
}

bool
XeGfxFramebuffer::startS3(IOService *provider)
{
    fS3 = true;
    setProperty("XeGfxS3", kOSBooleanTrue);
    logS3Gates("start");

    /*
     * readKapi()/kapiInit() only call the read-only v1 KAPI entry points
     * (getTiming/getConnectorInfo).  The scanout allocator is not called
     * unless the M3d expose arm is chosen and the preflight is green.
     */
    bool kapiOk = readKapi(provider);
    if (kapiOk && provider->getProperty("xegfx-external-dp") == kOSBooleanTrue) {
        struct XeGfxKmsPanelTiming current;
        uint32_t rc = XEGFX_ERR_NOT_READY;
        for (uint32_t n = 0; n < 120; n++) {
            bzero(&current, sizeof current);
            rc = fKapi->getTiming(fCtx, 0, &current);
            if (rc != XEGFX_ERR_NOT_READY)
                break;
            IOSleep(250);
        }
        kapiOk = rc == XEGFX_OK && buildMeasuredModeTable(&current, sizeof current);
        XLOG("external DP: committed native timing rc 0x%x, ready %d", rc, kapiOk ? 1 : 0);
    }
    kapiOk = kapiOk && kapiInit();
    if (kapiOk) {
        loadNubEdid(provider);
        buildK3Modes();
        fScalerOk = fK3 && (fKapi->flags & XEGFX_KMS_KAPI_FLAG_PLANE) && fKapi->reserved[3] != 0;
        if (fScalerOk)
            publishScalerCaps();
        else
            XLOG("A1: no scaled desktops (K3 %d, core setPlane %d)", fK3 ? 1 : 0,
                 (fKapi->flags & XEGFX_KMS_KAPI_FLAG_PLANE) ? 1 : 0);
    }
    if (!kapiOk) {
        XLOG("S3: check (a) FAILED during preflight; registering an INERT "
             "framebuffer -- open denied, no mode, no property, no NVRAM write "
             "beyond the start counter");
        if (!IOFramebuffer::start(provider))
            return false;
        fFbStarted = true;
        setProperty("XeGfxFbRegistered", kOSBooleanTrue);
        fInert     = true;
        setProperty("XeGfxDisplayInert", kOSBooleanTrue);
        setProperty("XeGfxDisplayVersion", "0.5-s3-degraded");
        setProperty("XeGfxS3Preflight", kOSBooleanFalse);
        setProperty("XeGfxS3Checks", (unsigned long long)0, 32);
        startS3Watchdog();
        return true;
    }

    fS3Checks = s3RunChecks("start/preflight");
    setProperty("XeGfxS3Checks", (unsigned long long)fS3Checks, 32);
    setProperty("XeGfxS3Preflight",
                (fS3Checks == XEGFX_S3_CHECKS_ALL) ? kOSBooleanTrue
                                                   : kOSBooleanFalse);
    if (fMeasuredModeCount > 0 && fMeasuredModes[0].refreshMHz >= 1000u)
        fRefreshHz = fMeasuredModes[0].refreshMHz / 1000u;

    /*
     * M3d: xegfx-disp-expose=1 upgrades this boot from "claim armed, open
     * denied" to a real serving framebuffer.  It requires the S3 preflight
     * (KAPI + timing agreement + detailed timing) plus the M3d-only checks
     * (contract, runnable mode, core scanout extension).  Anything short of
     * that leaves the S3 behaviour byte-for-byte unchanged.
     */
    fM3d = XEGFX_DISPLAY_M3D_ENABLE && (fExpose != 0) &&
           (fS3Checks == XEGFX_S3_CHECKS_ALL);
    setProperty("XeGfxM3dArmed", fM3d ? kOSBooleanTrue : kOSBooleanFalse);
    if (fM3d) {
        fM3dChecks = m3dRunChecks("start/preflight");
        setProperty("XeGfxM3dChecks", (unsigned long long)fM3dChecks, 32);
        if (fM3dChecks != XEGFX_M3D_CHECKS_ALL)
            XLOG("M3d: preflight 0x%x != 0x%x (%s); staying inert this boot",
                 fM3dChecks, XEGFX_M3D_CHECKS_ALL, fM3dWhy);
    }

    // IOFramebuffer::start uses this identity to create its controller. Late
    // publication leaves getWorkLoop() null until open() and sends early
    // display-pipe notification registration to the global graphics gate.
    if (fS3Checks == XEGFX_S3_CHECKS_ALL) {
        setProperty("IOFBDependentID", 0x5845474600000001ull, 64);
        setProperty("IOFBDependentIndex", (unsigned long long)fIndex, 32);
    }
    publishBacklight();
    XLOG("S3: IOFramebuffer::start BEGIN (dependent identity configured)");
    if (!IOFramebuffer::start(provider))
        return false;
    XLOG("S3: IOFramebuffer::start END");
    fFbStarted = true;
    setProperty("XeGfxDisplayVersion",
                fM3d ? "0.5.45-m3d" : "0.5.45-s3");

    /*
     * Arm the watchdog even when the preflight failed: no aperture + open
     * denied means this boot cannot display anything, so it must count as a
     * failed trial and eventually hand the panel back to XeGfxIntelFB.
     */
    startS3Watchdog();

    if (fM3d) {
        /* The KAPI scanout is mapped after the IOFramebuffer is registered:
         * it is pure memory/GGTT work, no display register is touched. */
        if (fM3dChecks == XEGFX_M3D_CHECKS_ALL && m3dAllocScanout()) {
            fM3dCandidate = true;
            k4ApplyBootState();
            if (!publishAgdcTiming())
                fM3dCandidate = false;
        }
        fWorkLoop = getWorkLoop();
        setProperty("XeGfxControllerReady", fWorkLoop ? kOSBooleanTrue : kOSBooleanFalse);
        XLOG("M3d: controller workloop %s before accel pairing", fWorkLoop ? "READY" : "MISSING");
        m3dLogState("start/preflight");
        XLOG("M3d: serving framebuffer armed (expose=1, checks 0x%x); open() "
             "releases only when the panel scans our buffer. XeGfxFbModeReady/"
             "FbReady are published from enableController, after open().",
             fM3dChecks);
    } else {
        /*
         * S3 keeps publishing here as well as in enableController().  Reason:
         * IOFramebuffer calls enableController() only AFTER its open()
         * succeeds, and S3's open() is a hard denial (expose is M3d).  Without
         * this, XeGfxFbModeReady and IOFBDetailedTimings could never be
         * observed in ioreg, and the whole point of S3 -- prove the mode table
         * is buildable without touching a register -- would be untestable.
         */
        if (fS3Checks == XEGFX_S3_CHECKS_ALL) {
            fFbModeReady = true;
            setProperty("XeGfxFbModeReady", kOSBooleanTrue);
            publishS3Timing();
            XLOG("S3: same-mode claim armed (phase=2 trial=1 fail=0): 1 "
                 "filtered mode, no aperture, open() denied, 0 register writes");
        } else
            XLOG("S3: same-mode claim armed but preflight=0x%x -- "
                 "enableController will return Unsupported and WindowServer "
                 "keeps the current output", fS3Checks);
    }

    /*
     * AGDC/accel pairing (lead request, 2026-10-28): head-4 slot dedup needs
     * a stable dependent identity.  These are inert property writes; the
     * gate-y XeGfxL0Ready is published from a delayed thread_call because
     * accel reacts to it by registering a display head, which is only safe
     * once start() has fully returned.
     */
    if (fS3Checks == XEGFX_S3_CHECKS_ALL) {
        s3L0PublishArm();
    }
    // Publish registration only after scanout preparation and dependent ID.
    setProperty("XeGfxFbRegistered", kOSBooleanTrue);
    XLOG("S3: start complete (candidate=%d); delayed L0 gate permits accel pairing",
         fM3dCandidate ? 1 : 0);
    return true;
}


void
XeGfxFramebuffer::startS3Watchdog()
{
#if !XEGFX_DISPLAY_S3_ENABLE
    return;
#else
    if (fWatchdog)
        return;
    fWatchdog = thread_call_allocate(&XeGfxFramebuffer::s3WatchdogCall, this);
    if (!fWatchdog) {
        XLOG("S3: thread_call_allocate failed; the start counter is disabled "
             "for this boot");
        return;
    }
    uint64_t deadline = 0;
    clock_interval_to_deadline(XEGFX_DISPLAY_WATCHDOG_SECONDS, kSecondScale,
                               &deadline);
    thread_call_enter1_delayed(fWatchdog, nullptr, deadline);
    XLOG("S3: watchdog armed for %u s; one successful open() cancels the "
         "start count", XEGFX_DISPLAY_WATCHDOG_SECONDS);
#endif
}

void
XeGfxFramebuffer::stopS3Watchdog()
{
    if (!fWatchdog)
        return;
    /*
     * thread_call_cancel_wait() is kernel-only: it exists in the kernel but is
     * not exported to kexts, so binding it makes kmutil refuse the whole kext
     * ("could not find a kext which exports this symbol").  Cancel, then free
     * with retries: a successful thread_call_free() already waits for a
     * callback that is still running.  Same pattern as NVRM's nvrm_power.cpp.
     * A free() that never succeeds leaks one thread_call, which is safer than
     * a use-after-free.
     */
    thread_call_cancel(fWatchdog);
    bool freed = false;
    for (int i = 0; i < 100; i++) {
        if (thread_call_free(fWatchdog)) {
            freed = true;
            break;
        }
        IOSleep(10);
        thread_call_cancel(fWatchdog);
    }
    if (!freed)
        XLOG("S3: thread_call still busy at stop; not freeing it (leak is "
             "safer than a use-after-free)");
    fWatchdog = nullptr;
}

void
XeGfxFramebuffer::s3WatchdogCall(thread_call_param_t p0, thread_call_param_t)
{
    XeGfxFramebuffer *self = (XeGfxFramebuffer *)p0;
    if (self)
        self->s3WatchdogFire();
}

void
XeGfxFramebuffer::s3WatchdogFire()
{
    if (fWatchdogFired)
        return;
    fWatchdogFired = true;

    uint32_t opens = 0, denied = 0;
    if (fGate)
        IOLockLock(fGate);
    opens  = fOpenCount;
    denied = fOpenDenied;
    if (fGate)
        IOLockUnlock(fGate);

    if (opens > 0) {
        XLOG("S3: watchdog: %u successful open(s) within %u s; not counting a "
             "failed start", opens, XEGFX_DISPLAY_WATCHDOG_SECONDS);
        return;
    }

    if (!fS3 || fS3Fail) {
        XLOG("S3: watchdog: claim no longer armed (s3=%d fail=%d); no NVRAM write",
             fS3 ? 1 : 0, fS3Fail ? 1 : 0);
        return;
    }

    uint32_t starts = 0;
    xgNvramU32(XEGFX_DISPLAY_NVRAM_STARTS, &starts);
    if (starts < XEGFX_DISPLAY_MAX_STARTS)
        starts++;

    if (!xgNvramSetU32(XEGFX_DISPLAY_NVRAM_STARTS, starts)) {
        XLOG("S3: watchdog: could not count the start (%s unchanged); the next "
             "boot will retry the same trial",
             XEGFX_DISPLAY_NVRAM_STARTS);
    } else {
        XLOG("S3: watchdog: no client opened the framebuffer in %u s "
             "(%u denied attempt(s)); %s=%u/%d",
             XEGFX_DISPLAY_WATCHDOG_SECONDS, denied,
             XEGFX_DISPLAY_NVRAM_STARTS, starts, XEGFX_DISPLAY_MAX_STARTS);
    }

    if (starts >= XEGFX_DISPLAY_MAX_STARTS) {
        if (xgNvramSetU32(XEGFX_DISPLAY_NVRAM_TRIAL, 0))
            XLOG("S3: watchdog: %d counted starts without a client; wrote %s=0 "
                 "-- XeGfxIntelFB returns on the next boot",
                 XEGFX_DISPLAY_MAX_STARTS, XEGFX_DISPLAY_NVRAM_TRIAL);
        else
            XLOG("S3: watchdog: starts exhausted but %s=0 write FAILED; the "
                 "next boot may claim again -- set it by hand",
                 XEGFX_DISPLAY_NVRAM_TRIAL);
    }
}

void
XeGfxFramebuffer::stop(IOService *provider)
{
    XLOG("stopping");

    if (fBacklight) {
        fBacklight->detach(this);
        OSSafeReleaseNULL(fBacklight);
    }

    /* Disarm the S3 watchdog first: after this point the object may go away
     * and the callback must not touch fOpenCount or NVRAM any more. */
    stopS3Watchdog();

    if (fL0Timer) {
        thread_call_cancel(fL0Timer);
        bool freed = false;
        for (int i = 0; i < 100; i++) {
            if (thread_call_free(fL0Timer)) {
                freed = true;
                break;
            }
            IOSleep(10);
            thread_call_cancel(fL0Timer);
        }
        if (!freed)
            XLOG("stop: L0 publish thread_call busy; leaking it");
        fL0Timer = nullptr;
    }

    m3dStopPresentRetry();
    if (fM3d)
        m3dStopVblankSource();
    stopVblankThread();
    if (fAccelObj) {
        fAccelObj->release();
        fAccelObj = nullptr;
    }

    /*
     * Never free the KAPI scanout here.  The display plane may still be
     * programmed to our buffer (latch is asynchronous); freeing would let the
     * display fetch freed pages (docs/16).  One GGTT window leaking on unload
     * is the safe trade-off for a trial driver.
     */
    if (fScanoutMapped)
        XLOG("stop: scanout handle 0x%x left allocated (plane safety)",
             fScanoutHandle);

    if (fVblTimer && fWorkLoop) {
        fVblTimer->cancelTimeout();
        fWorkLoop->removeEventSource(fVblTimer);
        fVblTimer->release();
        fVblTimer = nullptr;
    }
    fVblHW = false;
    fVblCookie = nullptr;
    fVblProc = nullptr;
    fVblTarget = nullptr;
    fVblRef = nullptr;
    fOwnsHead = false;

    if (fFbStarted) {
        IOFramebuffer::stop(provider);
        fFbStarted = false;
    } else {
        /* M2b skeleton: started with IOService::start(), never IOFramebuffer. */
        IOService::stop(provider);
    }
}

IOReturn
XeGfxFramebuffer::open()
{
    /*
     * M3d: the only path that releases the framebuffer.  Three gates, all
     * pre-validated at startS3(): the expose arm, the full preflight, and a
     * mapped KAPI scanout whose first present the accel accepted (so the
     * panel really scans the buffer WindowServer is about to draw into).
     * Anything else stays denied, which the S3 watchdog counts as a failed
     * trial and turns into a rollback to XeGfxIntelFB.
     */
    if (fS3 && fM3d) {
        const char *why = nullptr;

        if (!m3dGateEnabled())
            why = "gate-closed";
        else if (!fM3dCandidate)
            why = "no-scanout";
        else if (!m3dApertureReady())
            why = "aperture-not-mapped";
        else {
            /* The accel may publish its property later than us; the first
             * present is the real proof.  Retry briefly (thread context, a
             * few 5 ms sleeps are fine here). */
            (void)m3dFindAccel(true);
            for (int i = 0; i < 8 && !fPresented; i++) {
                if (i > 0)
                    IOSleep(100);
                (void)m3dEnsurePresented();
            }
            if (!fPresented)
                why = "present-failed";
        }

        if (why != nullptr) {
            uint32_t denied = ++fOpenDenied;
            setProperty("XeGfxFbOpened", kOSBooleanFalse);
            setProperty("XeGfxFbOpenDenied", (unsigned long long)denied, 32);
            setProperty("XeGfxM3dOpenDeniedReason", why);
            /* No asynchronous present retry after a denial: a late successful
             * present would switch the panel to a scanout nobody has drawn
             * into yet (black), while open stays denied.  The next open()
             * attempt (or the S3 watchdog rollback) is the safe recovery. */
            XLOG("M3d: open() denied (%s, checks 0x%x lastRc 0x%x, denied=%u) "
                 "-- panel keeps the firmware frame; the watchdog will count "
                 "this boot as failed", why, fM3dChecks, fLastPresentRc, denied);
            m3dLogState("open/deny");
            return kIOReturnExclusiveAccess;
        }

        IOReturn rc = IOFramebuffer::open();
        if (rc == kIOReturnSuccess) {
            /*
             * Defensive: aperture is only served when fM3dReady is set, and
             * that normally happens in enableController() (the base class
             * calls it from postOpen).  If a future IOFramebuffer revises
             * that order, a successful open with a mapped scanout and an
             * accepted present is equivalent evidence that the panel scans
             * our buffer, so promote it here rather than serve NULL.
             */
            if (!fM3dReady && fM3dCandidate && fPresented &&
                m3dApertureReady()) {
                fM3dReady = true;
                setProperty("XeGfxM3dReady", kOSBooleanTrue);
                XLOG("M3d: open() promoted ready (enableController not seen)");
            }
            uint32_t opens = 0;
            if (fGate)
                IOLockLock(fGate);
            fOpenCount++;
            opens = fOpenCount;
            fOpened = true;
            if (fGate)
                IOLockUnlock(fGate);
            setProperty("XeGfxFbOpened", kOSBooleanTrue);
            setProperty("XeGfxFbOpenCount", (unsigned long long)opens, 32);
            setProperty("XeGfxM3dOpened", kOSBooleanTrue);
            XLOG("M3d: open() RELEASED (opens=%u) -- aperture 0x%llx/%llu live, "
                 "head 4 scans our buffer", opens,
                 (unsigned long long)fAperturePhys,
                 (unsigned long long)fApertureLen);
            m3dLogState("open/ok");
        } else
            XLOG("M3d: IOFramebuffer::open() returned 0x%x", rc);
        return rc;
    }

    if (fS3) {
        uint32_t denied = ++fOpenDenied;
        setProperty("XeGfxFbOpened", kOSBooleanFalse);
        setProperty("XeGfxFbOpenDenied", (unsigned long long)denied, 32);
        if (fM3d)
            XLOG("S3: open() denied (M3d armed but not candidate, checks 0x%x, "
                 "why=%s, denied=%u)", fM3dChecks, fM3dWhy, denied);
        else
            XLOG("S3: open() denied (expose=%u, no M3d serve this boot); the "
                 "watchdog will count this boot as a failed start (denied=%u)",
                 fExpose, denied);
        logS3Gates("open");
        return kIOReturnExclusiveAccess;
    }

    /* M3c S2: an inert framebuffer must never be opened. */
    if (fInert) {
        uint32_t denied = ++fOpenDenied;
        setProperty("XeGfxFbOpened", kOSBooleanFalse);
        setProperty("XeGfxFbOpenDenied", (unsigned long long)denied, 32);
        XLOG("S2: open() denied (inert framebuffer, denied=%u)", denied);
        return kIOReturnExclusiveAccess;
    }

    IOReturn rc = IOFramebuffer::open();
    if (rc == kIOReturnSuccess) {
        /* Only a real client open feeds the S3 watchdog and the M4a claim. */
        uint32_t opens = 0;
        if (fGate)
            IOLockLock(fGate);
        fOpenCount++;
        opens = fOpenCount;
        fOpened = true;
        if (fGate)
            IOLockUnlock(fGate);

        /* M4a: latch the fact for XeGfxFBClaim's boot-chain poller. */
        setProperty("XeGfxFbOpened", kOSBooleanTrue);
        setProperty("XeGfxFbOpenCount", (unsigned long long)opens, 32);
    }
    return rc;
}

void
XeGfxFramebuffer::close()
{
    if (fS3) {
        if (fOpened) {
            fOpened = false;
            IOFramebuffer::close();
        }
        return;
    }
    if (!fInert)
        IOFramebuffer::close();
}

void
XeGfxFramebuffer::free()
{
    /* stop() normally freed it; free() can be reached without stop() when
     * start() failed half-way, so be defensive. */
    stopS3Watchdog();
    OSSafeReleaseNULL(fBacklight);

    if (fL0Timer) {
        thread_call_cancel(fL0Timer);
        if (thread_call_free(fL0Timer))
            fL0Timer = nullptr;
        else
            XLOG("free: L0 publish thread_call busy; leaking it");
    }
    if (fPresentTimer) {
        thread_call_cancel(fPresentTimer);
        if (thread_call_free(fPresentTimer))
            fPresentTimer = nullptr;
        else
            XLOG("free: present retry thread_call busy; leaking it");
    }
    if (fAccelObj) {
        fAccelObj->release();
        fAccelObj = nullptr;
    }

    if (fIoModeIds)    { IOFree(fIoModeIds, fIoModeCount * sizeof *fIoModeIds); fIoModeIds = nullptr; }
    if (fIoCfgModeIdx) { IOFree(fIoCfgModeIdx, fIoModeCount * sizeof *fIoCfgModeIdx); fIoCfgModeIdx = nullptr; }
    if (fAperture)     { fAperture->release(); fAperture = nullptr; }
    if (fGamma)        { IOFree(fGamma, fGammaLen); fGamma = nullptr; fGammaLen = 0; }
    if (fGate)         { IOLockFree(fGate); fGate = nullptr; }
    if (fCursorLock)   { IOLockFree(fCursorLock); fCursorLock = nullptr; }
    stopVblankThread();
    if (fVblLock)      { IOLockFree(fVblLock); fVblLock = nullptr; }
    IOFramebuffer::free();
}

/* ======================================================================= */
/* M2b: the PCI retire personality (XeGfxFBClaim)                          */
/* ======================================================================= */

/*
 * M3c S3: the category claim (docs/12 §4.4).  Freeze: once this personality
 * wins the IOFramebuffer category for the PCI display, XeGfxIntelFB is not
 * matched again in the same boot and there is no reliable way to hand the
 * category back.  So the claim is allowed ONLY in the fully armed, human
 * opt-in state -- phase==2 && trial==1 && fail==0 && starts<3 -- and declines
 * with the reason in the log for every other combination.  A decline leaves
 * the IntelFB placeholder exactly as it is.
 */
/* The core consumes the trial token; its resource is valid only in this boot. */
static bool
xgTrialOnceAllowed()
{
    IORegistryEntry *o = IORegistryEntry::fromPath("/options", gIODTPlane);
    if (!o) return false;
    OSObject *value = o->copyProperty("xegfx-disp-once");
    o->release();
    if (!value) return true;
    OSData *d = OSDynamicCast(OSData, value);
    uint32_t armed = 0;
    if (d && d->getLength() == sizeof armed)
        memcpy(&armed, d->getBytesNoCopy(), sizeof armed);
    value->release();
    if (armed == 1) return true;
    for (unsigned n = 0; n < 20; n++) {
        IOService *resources = IOService::getResourceService();
        if (resources && resources->getProperty("WTLArrowTrialActive") == kOSBooleanTrue)
            return true;
        IOSleep(1);
    }
    return false;
}

static bool
xgS3ClaimArmed(const char *caller)
{
#if !XEGFX_DISPLAY_S3_ENABLE
    XLOG("claim %s: S3 compiled out (XEGFX_DISPLAY_S3_ENABLE=0); declining",
         caller);
    return false;
#else
    uint32_t phase = 0, trial = 0, fail = 0, starts = 0, expose = 0;

    if (!xgTrialOnceAllowed()) {
        XLOG("claim %s: one-shot trial exhausted; firmware framebuffer retained", caller);
        return false;
    }
    if (!xgNvramU32(XEGFX_DISPLAY_NVRAM_PHASE, &phase) ||
        !xgNvramU32(XEGFX_DISPLAY_NVRAM_TRIAL, &trial)) {
        XLOG("claim %s: no xegfx-disp-phase/trial in NVRAM; declining, "
             "IntelFB keeps the panel", caller);
        return false;
    }
    xgNvramU32(XEGFX_DISPLAY_NVRAM_FAIL,   &fail);
    xgNvramU32(XEGFX_DISPLAY_NVRAM_STARTS, &starts);
    xgNvramU32(XEGFX_DISPLAY_NVRAM_EXPOSE, &expose);

    if (phase != 2) {
        XLOG("claim %s: phase=%u != 2; declining (S2/S1 keeps IntelFB)",
             caller, phase);
        return false;
    }
    if (trial != 1) {
        XLOG("claim %s: trial=%u != 1; declining -- freeze: the category "
             "cannot be yielded back in this boot", caller, trial);
        return false;
    }
    if (fail != 0) {
        XLOG("claim %s: xegfx-disp-fail=1 (manual pull); declining, IntelFB "
             "keeps the panel", caller);
        return false;
    }
    if (starts >= XEGFX_DISPLAY_MAX_STARTS) {
        XLOG("claim %s: xegfx-disp-starts=%u >= %d; declining",
             caller, starts, XEGFX_DISPLAY_MAX_STARTS);
        return false;
    }

    XLOG("claim %s: phase=2 trial=1 fail=0 starts=%u expose=%u -- claiming "
         "the IOFramebuffer category (score 20100); no yield back this boot",
         caller, starts, expose);
    return true;
#endif
}

/*
 * M4a boot-chain observability.  XeGfxFBClaim is the only adapter object that
 * is guaranteed to exist this early, so it polls three read-only facts and
 * publishes the result on its own registry node.  serviceMatching() and
 * getMatchingServices() both return +1 references in this build
 * (IOKIT_ENABLE_SHARED_PTR is unset), so release them here.
 */
static bool
xgClaimBoolProperty(IOService *service, const char *key)
{
    if (!service)
        return false;

    OSObject *prop = service->getProperty(key);
    if (OSBoolean *b = OSDynamicCast(OSBoolean, prop))
        return b->getValue();
    if (OSNumber *n = OSDynamicCast(OSNumber, prop))
        return n->unsigned32BitValue() != 0;
    if (OSData *d = OSDynamicCast(OSData, prop)) {
        if (d->getLength() >= 1 && d->getBytesNoCopy())
            return *(const uint8_t *)d->getBytesNoCopy() != 0;
    }
    return false;
}

static bool
xgClaimResourceBool(const char *key)
{
    IOService *resources = IOService::getResourceService();
    return resources ? xgClaimBoolProperty(resources, key) : false;
}

static bool
xgClaimResourcePresent(const char *key)
{
    IOService *resources = IOService::getResourceService();
    return resources && resources->getProperty(key) != nullptr;
}

static uint32_t
xgClaimCountServices(const char *className)
{
    OSDictionary *matching = IOService::serviceMatching(className);
    if (!matching)
        return 0;

    OSIterator *iter = IOService::getMatchingServices(matching);
    matching->release();
    if (!iter)
        return 0;

    uint32_t count = 0;
    while (OSObject *obj = iter->getNextObject()) {
        if (OSDynamicCast(IOService, obj))
            count++;
    }
    iter->release();
    return count;
}

struct XgClaimFbScan {
    uint32_t nodes;
    uint32_t registered;
    uint32_t opened;
    uint32_t opens;
    uint32_t denied;
};

static void
xgClaimScanFramebuffers(XgClaimFbScan *out)
{
    memset(out, 0, sizeof(*out));

    OSDictionary *matching = IOService::serviceMatching("XeGfxFramebuffer");
    if (!matching)
        return;

    OSIterator *iter = IOService::getMatchingServices(matching);
    matching->release();
    if (!iter)
        return;

    while (OSObject *obj = iter->getNextObject()) {
        IOService *service = OSDynamicCast(IOService, obj);
        if (!service)
            continue;

        out->nodes++;
        if (xgClaimBoolProperty(service, "XeGfxFbRegistered"))
            out->registered++;

        uint32_t opens = xgUIntProperty(service, "XeGfxFbOpenCount", 0);
        bool opened = xgClaimBoolProperty(service, "XeGfxFbOpened") || opens > 0;
        if (opened)
            out->opened++;
        out->opens += opens;
        out->denied += xgUIntProperty(service, "XeGfxFbOpenDenied", 0);
    }
    iter->release();
}

enum {
    kXeGfxClaimStateClaimed = 0,
    kXeGfxClaimStateWaitingCore,
    kXeGfxClaimStateWaitingNub,
    kXeGfxClaimStateWaitingFb,
    kXeGfxClaimStateReady,
    kXeGfxClaimStateFailed
};

static const char *
xgClaimStateName(uint32_t state)
{
    switch (state) {
    case kXeGfxClaimStateClaimed:     return "claimed";
    case kXeGfxClaimStateWaitingCore: return "waiting-core";
    case kXeGfxClaimStateWaitingNub:  return "waiting-nub";
    case kXeGfxClaimStateWaitingFb:   return "waiting-fb";
    case kXeGfxClaimStateReady:       return "ready";
    case kXeGfxClaimStateFailed:      return "failed";
    default:                          return "unknown";
    }
}

static const uint32_t kXeGfxClaimPollSeconds = 5;
static const uint32_t kXeGfxClaimPollMax     = 24;   /* 24 * 5 s = 120 s */

class XeGfxFBClaim : public IOService
{
    OSDeclareDefaultStructors(XeGfxFBClaim)

public:
    virtual bool       init(OSDictionary *dictionary = 0) APPLE_KEXT_OVERRIDE;
    virtual IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
    virtual bool       start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void       stop(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void       free() APPLE_KEXT_OVERRIDE;

private:
    static void pollCall(thread_call_param_t p0, thread_call_param_t p1);
    void        startPoll();
    void        stopPoll();
    void        pollTick();
    void        finishReady(uint32_t t);
    void        finishFailed(uint32_t t, const char *reason);

    thread_call_t  fPoll;
    uint32_t       fPollCount;
    uint32_t       fState;
    bool           fTerminal;
    volatile bool  fStopped;
};

OSDefineMetaClassAndStructors(XeGfxFBClaim, IOService)

bool
XeGfxFBClaim::init(OSDictionary *dictionary)
{
    fPoll      = nullptr;
    fPollCount = 0;
    fState     = kXeGfxClaimStateClaimed;
    fTerminal  = false;
    fStopped   = false;
    return IOService::init(dictionary);
}

IOService *
XeGfxFBClaim::probe(IOService *provider, SInt32 *score)
{
    if (!xgS3ClaimArmed("probe"))
        return nullptr;

    /* Info.plist says 20100; keep it if the kernel passed a lower default.
     * Never lower a score set by matching. */
    if (score && *score < 20100)
        *score = 20100;

    XLOG("claim: probe accepted %s (score 20100 > IntelFB 20001)",
         provider ? provider->getName() : "(no provider)");
    return this;
}

bool
XeGfxFBClaim::start(IOService *provider)
{
    /*
     * Defensive re-check: probe() is the only path that should ever accept,
     * but a wrong claim cannot be undone before the next boot, so verify the
     * gate twice rather than trust the earlier decision.
     */
    if (!xgS3ClaimArmed("start"))
        return false;
    if (!IOService::start(provider))
        return false;

    /* Property only: this service holds the category, it is not a second
     * IOFramebuffer and does not change the framebuffer count by itself. */
    setProperty("XeGfxFBClaimActive", kOSBooleanTrue);

    /*
     * M4a: arm the boot chain.  Both resources live on IOResources.  Pass an
     * explicit kOSBooleanTrue instead of relying on publishResource(key,
     * NULL): the IOService.h contract does not define the NULL default, and
     * the core's IOResourceMatch personality needs a real property value.
     * XeGfxBootArm is the marker the core checks before running stages 1..7;
     * XeGfxArm is the resource the gated personality itself matches.
     */
    IOService::publishResource("XeGfxBootArm", kOSBooleanTrue);
    XLOG("claim: published IOResources XeGfxBootArm=true (boot chain armed)");
    IOService::publishResource("XeGfxArm", kOSBooleanTrue);
    XLOG("claim: published IOResources XeGfxArm=true "
         "(core gated personality trigger)");

    /* Presence, not value: core _start re-publishes XeGfxArm with the
     * IOService sentinel when it loads, so a boolean read turns false. */
    bool arm     = xgClaimResourcePresent("XeGfxArm");
    bool bootArm = xgClaimResourcePresent("XeGfxBootArm");
    setProperty("xegfx-fb-claim-arm",
                arm ? kOSBooleanTrue : kOSBooleanFalse);
    setProperty("xegfx-fb-claim-bootarm",
                bootArm ? kOSBooleanTrue : kOSBooleanFalse);
    setProperty("xegfx-fb-claim-state", "claimed");
    setProperty("xegfx-fb-claim-t", (unsigned long long)0, 32);
    setProperty("xegfx-fb-claim-polls", (unsigned long long)0, 32);
    setProperty("xegfx-fb-claim-chain", kOSBooleanFalse);
    setProperty("xegfx-fb-claim-reason", "armed");
    setProperty("xegfx-fb-claim-poll", "pending");

    XLOG("claim-start: state=claimed budget=%us poll=%us arm=%u bootarm=%u -- "
         "holding the IOFramebuffer category; XeGfxIntelFB must not match in "
         "this boot (freeze)",
         kXeGfxClaimPollMax * kXeGfxClaimPollSeconds,
         kXeGfxClaimPollSeconds, arm ? 1 : 0, bootArm ? 1 : 0);

    registerService();

    /* Never block here: the core loads asynchronously and the 120 s budget is
     * measured by the thread_call, not by start().  registerService() first so
     * the node (and xegfx-fb-claim-state=claimed) is visible before poll #1. */
    startPoll();
    return true;
}

void
XeGfxFBClaim::startPoll()
{
    fPoll = thread_call_allocate(&XeGfxFBClaim::pollCall, this);
    if (!fPoll) {
        XLOG("claim: thread_call_allocate failed; claiming continues but "
             "boot-chain polling is unavailable");
        finishFailed(0, "poll-unavailable");
        setProperty("xegfx-fb-claim-poll", "unavailable");
        return;
    }

    uint64_t deadline = 0;
    clock_interval_to_deadline(kXeGfxClaimPollSeconds, kSecondScale, &deadline);
    /* thread_call_enter* returns TRUE when the call was already pending; it
     * has no failure return.  A fresh call is armed by this call. */
    (void)thread_call_enter1_delayed(fPoll, nullptr, deadline);

    setProperty("xegfx-fb-claim-poll", "armed");
    XLOG("claim: poll armed t=%us every %us up to %us (24 checkpoints)",
         kXeGfxClaimPollSeconds, kXeGfxClaimPollSeconds,
         kXeGfxClaimPollMax * kXeGfxClaimPollSeconds);
}

void
XeGfxFBClaim::stopPoll()
{
    if (!fPoll)
        return;

    thread_call_cancel(fPoll);
    bool freed = false;
    for (int i = 0; i < 100; i++) {
        if (thread_call_free(fPoll)) {
            freed = true;
            break;
        }
        IOSleep(10);
        thread_call_cancel(fPoll);
    }
    if (!freed)
        XLOG("claim: thread_call busy at stop; leaking it rather than risking "
             "a use-after-free");
    fPoll = nullptr;
}

void
XeGfxFBClaim::pollCall(thread_call_param_t p0, thread_call_param_t)
{
    XeGfxFBClaim *self = (XeGfxFBClaim *)p0;
    if (self)
        self->pollTick();
}

void
XeGfxFBClaim::pollTick()
{
    if (fStopped || fTerminal)
        return;

    fPollCount++;
    const uint32_t t = fPollCount * kXeGfxClaimPollSeconds;

    const uint32_t coreNodes = xgClaimCountServices("XeGfxResident");
    const bool coreProp      = xgClaimResourceBool("XeGfxCoreReady");
    const bool arm           = xgClaimResourcePresent("XeGfxArm");
    const bool bootArm       = xgClaimResourcePresent("XeGfxBootArm");
    const bool core          = (coreNodes > 0) || coreProp;
    const uint32_t nubs      = xgClaimCountServices("XeGfxDisplayNub");

    XgClaimFbScan fb;
    xgClaimScanFramebuffers(&fb);

    const bool opened     = (fb.opened > 0) || (fb.opens > 0);
    /* A XeGfxFramebuffer node only exists once IOFramebuffer::start() has
     * registered it; the property is our explicit latch and is true on every
     * M4a-aware build.  Accept either so the poll also understands a node
     * registered by an adapter build that predates the latch. */
    const bool registered = (fb.registered > 0) || (fb.nodes > 0);
    const bool chain      = core && (nubs > 0) && registered;

    if (opened)
        fState = kXeGfxClaimStateReady;
    else if (!core)
        fState = kXeGfxClaimStateWaitingCore;
    else if (nubs == 0)
        fState = kXeGfxClaimStateWaitingNub;
    else
        fState = kXeGfxClaimStateWaitingFb;

    setProperty("xegfx-fb-claim-state", xgClaimStateName(fState));
    setProperty("xegfx-fb-claim-t", (unsigned long long)t, 32);
    setProperty("xegfx-fb-claim-polls", (unsigned long long)fPollCount, 32);
    setProperty("xegfx-fb-claim-core", (unsigned long long)coreNodes, 32);
    setProperty("xegfx-fb-claim-core-ready",
                coreProp ? kOSBooleanTrue : kOSBooleanFalse);
    setProperty("xegfx-fb-claim-nub", (unsigned long long)nubs, 32);
    setProperty("xegfx-fb-claim-fb", (unsigned long long)fb.nodes, 32);
    setProperty("xegfx-fb-claim-registered",
                (unsigned long long)fb.registered, 32);
    setProperty("xegfx-fb-claim-opened", (unsigned long long)fb.opened, 32);
    setProperty("xegfx-fb-claim-open-count",
                (unsigned long long)fb.opens, 32);
    setProperty("xegfx-fb-claim-denied", (unsigned long long)fb.denied, 32);
    setProperty("xegfx-fb-claim-chain",
                chain ? kOSBooleanTrue : kOSBooleanFalse);
    setProperty("xegfx-fb-claim-arm",
                arm ? kOSBooleanTrue : kOSBooleanFalse);
    setProperty("xegfx-fb-claim-bootarm",
                bootArm ? kOSBooleanTrue : kOSBooleanFalse);

    XLOG("claim-poll: t=%us state=%s core=%u coreprop=%u nub=%u fb=%u "
         "registered=%u opened=%u opens=%u denied=%u chain=%u arm=%u",
         t, xgClaimStateName(fState), coreNodes, coreProp ? 1 : 0, nubs,
         fb.nodes, fb.registered, fb.opened, fb.opens, fb.denied,
         chain ? 1 : 0, arm ? 1 : 0);

    if (fState == kXeGfxClaimStateReady) {
        finishReady(t);
        return;
    }

    if (fPollCount >= kXeGfxClaimPollMax) {
        const char *reason = "unknown";
        if (!core)
            reason = "core-not-loaded";
        else if (nubs == 0)
            reason = "nub-not-published";
        else if (fb.nodes == 0)
            reason = "fb-service-absent";
        else if (!registered)
            reason = "fb-not-registered";
        else if (!opened)
            reason = "fb-not-opened";
        finishFailed(t, reason);
        return;
    }

    uint64_t deadline = 0;
    clock_interval_to_deadline(kXeGfxClaimPollSeconds, kSecondScale, &deadline);
    /* Re-arming from our own callback is supported; the callback is not
     * considered pending while it runs, so this schedules exactly one next
     * invocation five seconds from now. */
    (void)thread_call_enter1_delayed(fPoll, nullptr, deadline);
}

void
XeGfxFBClaim::finishReady(uint32_t t)
{
    fTerminal = true;
    setProperty("xegfx-fb-claim-state", "ready");
    setProperty("xegfx-fb-claim-reason", "opened");
    setProperty("xegfx-fb-claim-poll", "done");
    XLOG("claim-final: t=%us state=ready reason=opened -- adapter framebuffer "
         "is registered and opened; boot chain complete", t);
}

void
XeGfxFBClaim::finishFailed(uint32_t t, const char *reason)
{
    fState    = kXeGfxClaimStateFailed;
    fTerminal = true;
    setProperty("xegfx-fb-claim-state", "failed");
    setProperty("xegfx-fb-claim-reason", reason);
    setProperty("xegfx-fb-claim-t", (unsigned long long)t, 32);
    setProperty("xegfx-fb-claim-poll", "done");
    XLOG("claim-final: t=%us state=failed reason=%s -- no NVRAM write; "
         "userspace bootguard owns rollback/next-boot decision", t, reason);
}

void
XeGfxFBClaim::stop(IOService *provider)
{
    fStopped = true;
    stopPoll();
    IOService::stop(provider);
}

void
XeGfxFBClaim::free()
{
    fStopped = true;
    stopPoll();
    IOService::free();
}
