/*
 * XeGfxDisplay.h -- NVRMFB-style IOFramebuffer adapter over the XeGfx KMS
 *                   engine (i915 display).
 *
 * Object model, mirroring NVRMFB exactly:
 *
 *   XeGfx.kext
 *     +-- i915 display/KMS engine
 *     +-- one "XeGfxDisplayNub" IOService per connector, published after
 *         intel_display_driver_register(), carrying
 *              "fb-index"      = <u32 index>
 *              "xegfx-kms-kapi"= <OSData: struct XeGfxKmsKapi>
 *              "xegfx-timing"  = <u32[13]>  (AGDC panel timing, see XeGfxAGDC)
 *
 *   XeGfxDisplay.kext
 *     +-- XeGfxFramebuffer : IOFramebuffer      <- this file
 *
 * We subclass IOFramebuffer directly, NOT IONDRVFramebuffer.  IONDRVFramebuffer
 * models "describe the raster the firmware already lit and never change it" --
 * that is precisely why the placeholder XeGfxIntelFB can never do 240 Hz or
 * HiDPI.  Every mode change has to go through enableController()/setDisplayMode()
 * here and out to intel_atomic_commit() in the core kext.
 */

#ifndef _XEGFXDISPLAY_H_
#define _XEGFXDISPLAY_H_

#include <IOKit/IOService.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/graphics/IOGraphicsTypes.h>
#include <kern/thread_call.h>          /* M3c S3: 120 s start watchdog      */

#include "xegfx_kms_kapi.h"

#define XEGFX_DISPLAY_NAME  "XeGfxDisplay"

/*
 * M2b acceptance requires the IOLog prefix to be exactly "XEGFX_DISPLAY:".
 * Keep it separate from XEGFX_DISPLAY_NAME so the bundle name is unchanged.
 */
#define XEGFX_DISPLAY_LOG_PREFIX  "XEGFX_DISPLAY: "

/*
 * Boot-arg / NVRAM switches.  Every failure mode below has an off switch.
 *
 * M2b changes the master switch to "xedisp" and it defaults OFF: a literal
 * "-xedisp=1" (or "xedisp=1") boot-arg is required before probe() will claim
 * the XeGfxDisplayNub.  Without it this kext matches nothing and the firmware
 * raster/placeholder driver stays in charge.
 *
 * The argument *name* never carries "-" or "=value"; xgBootArgEnabled() tries
 * both PE_parse_boot_argn() spellings because XNU's leading-dash handling has
 * changed across releases (XeGfxIntelFB relies on the dashed form).
 */
#define XEGFX_DISPLAY_BOOTARG_ENABLE    "xedisp"       /* -xedisp=1 required   */
#define XEGFX_DISPLAY_BOOTARG_NOMODE    "xedispdt"     /* xedispdt=0: no modeset */
#define XEGFX_DISPLAY_BOOTARG_NOACCEL   "xedispaccel"  /* xedispaccel=0        */

/*
 * M3c two-stage arming gates (docs/12 §4.4).  They live in the device tree
 * /options node because OpenCore rewrites boot-args on every boot.
 *   xegfx-disp-phase   0 off, 1 inert registration (S2), 2 same-mode claim (S3)
 *   xegfx-disp-trial   1 allows XeGfxFBClaim to take the IOFramebuffer category
 *   xegfx-disp-fail    1 = manual pull: every FB-count-changing action declines
 *   xegfx-disp-expose  0/1 open() permission.  Before M3d this is logged only:
 *                      S3 keeps open() denied because there is no aperture.
 *                      M3d honours it only when the full preflight below is
 *                      green; every other phase ignores it entirely.
 */
#define XEGFX_DISPLAY_NVRAM_PHASE       "xegfx-disp-phase"
#define XEGFX_DISPLAY_NVRAM_TRIAL       "xegfx-disp-trial"
#define XEGFX_DISPLAY_NVRAM_FAIL        "xegfx-disp-fail"
#define XEGFX_DISPLAY_NVRAM_EXPOSE      "xegfx-disp-expose"
#define XEGFX_DISPLAY_NVRAM_STARTS      "xegfx-disp-starts"

/*
 * Guard: how many counted starts before we stop taking the display over.
 * Same idea as NVRMFB's xeintelfb-starts, which exists because a broken
 * modeset leaves the machine unusable except through single-user mode.
 */
#define XEGFX_DISPLAY_MAX_STARTS  3

/*
 * M3c S3 compile-time interlock.  With this at 0 the whole S3 path -- claim,
 * preflight, watchdog and the NVRAM writes -- is compiled out; the adapter
 * behaves exactly like the S2 build.  It is a second line of defence behind
 * the runtime `phase == 2 && trial == 1` gate: a logic bug in the gate alone
 * still cannot cause an S3 side effect on a machine that has not opted in.
 */
#ifndef XEGFX_DISPLAY_S3_ENABLE
#define XEGFX_DISPLAY_S3_ENABLE  1
#endif

/* docs/12 §4.5: a start that never sees a client is counted after this long. */
#define XEGFX_DISPLAY_WATCHDOG_SECONDS  120

/* The three preflight checks of docs/12 §4.2 / §7 S3, as a bitmask. */
enum {
    XEGFX_S3_CHECK_KAPI   = 1u << 0,   /* (a) nub publishes the KAPI table   */
    XEGFX_S3_CHECK_TIMING = 1u << 1,   /* (b) measured == KAPI current mode  */
    XEGFX_S3_CHECK_DT     = 1u << 2,   /* (c) 13 words -> one detail timing  */
    XEGFX_S3_CHECKS_ALL   = (1u << 3) - 1u
};

/*
 * ---- M3a: NVK5 panel timing words ----------------------------------------
 *
 * The core nub publishes "measured-panel-timing" as 13 little-endian u32
 * words.  That is the NVK5 timing record of the NVRM AGDC shim, whose
 * canonical definition lives in the reference tree at
 *   src/NvGfx-pre-035-20261024/kexts/NVRMAGDC/nvrm-agdc-k5.h
 * (#define NVK5_TIMING_TAG 0x4b350001u, NVK5_TIMING_WORDS 13, enum NVK5_T_*).
 * The core fills it in xegfxPanelTiming() (panel-timing.h) from the live i915
 * transcoder registers plus a frame-counter measurement, so word 1 is a
 * *measured* raster clock, not an EDID or PLL readout.
 *
 * We keep local index names instead of including the NVRM header: this kext
 * must not depend on the core tree, and the ABI is just 13 u32.
 */
#define XEGFX_NVK5_TIMING_TAG    0x4b350001u
#define XEGFX_NVK5_TIMING_WORDS  13
enum {
    XEGFX_NVK5_T_TAG = 0,      /* 0x4b350001 magic                           */
    XEGFX_NVK5_T_PCLK,         /* pixel clock, Hz (measured)                 */
    XEGFX_NVK5_T_HVIS,         /* horizontal visible (active) pixels         */
    XEGFX_NVK5_T_HSS,          /* horizontal sync start (absolute)           */
    XEGFX_NVK5_T_HSE,          /* horizontal sync end (absolute)             */
    XEGFX_NVK5_T_HTOT,         /* horizontal total                           */
    XEGFX_NVK5_T_VVIS,         /* vertical visible (active) lines            */
    XEGFX_NVK5_T_VSS,          /* vertical sync start (absolute)             */
    XEGFX_NVK5_T_VSE,          /* vertical sync end (absolute)               */
    XEGFX_NVK5_T_VTOT,         /* vertical total                             */
    XEGFX_NVK5_T_REFRESH_MHZ,  /* measured refresh, milli-Hz                 */
    XEGFX_NVK5_T_HSYNCPOS,     /* 1 = positive hsync polarity                */
    XEGFX_NVK5_T_VSYNCPOS      /* 1 = positive vsync polarity                */
};

/*
 * One read-only mode record built from the words above.  Kept separate from
 * fConn.modes/fIoModeIds on purpose: M3a must not touch the KAPI mode table
 * that IOFramebuffer would publish, it only proves the timing can be turned
 * into IOFramebuffer structures.
 */
#define XEGFX_DISPLAY_MAX_MEASURED_MODES  4
struct XeGfxMeasuredMode {
    IODisplayModeID              modeId;
    uint64_t                     pixelClockHz;  /* exact; timingToIo is kHz */
    uint32_t                     refreshMHz;    /* milli-Hz, from word 10    */
    struct XeGfxKmsTiming        t;             /* reused KAPI timing struct */
    IODetailedTimingInformationV2 dt;
    IODisplayModeInformation     info;
    IOPixelInformation           px;            /* 32bpp long-word RGB       */
    bool                         scaled;        /* HiDPI candidate, inactive */
};

/*
 * ---- M3d: a real, serving IOFramebuffer ---------------------------------
 *
 * docs/12 §3 requires eight pure-virtual IOFramebuffer methods plus the
 * PlatformFunction tier; M3d fills in the scanout/aperture/flip/vblank half
 * so WindowServer can actually draw into a buffer the display scans out.
 *
 * Nothing here is reachable unless ALL of the following hold, re-checked at
 * each entry point (docs/17 §B1 "adapter hard checks"):
 *   - the compile-time interlock XEGFX_DISPLAY_M3D_ENABLE is 1;
 *   - phase == 2 && trial == 1 && fail == 0 (i.e. fS3) and starts < 3;
 *   - NVRAM xegfx-disp-expose == 1;
 *   - the nub contract is valid (v1 KAPI table, vbt-child-devices > 0);
 *   - KAPI getTiming() is READY and the measured mode was converted to a
 *     detailed timing that IOFramebuffer accepted;
 *   - the core advertises the M3d scanout extension and a scanout is mapped.
 * Any failure leaves the adapter in its pre-M3d S3 behaviour: open() denied,
 * no aperture, no scanout, no display register touched.
 */
#ifndef XEGFX_DISPLAY_M3D_ENABLE
#define XEGFX_DISPLAY_M3D_ENABLE  1
#endif

/* M3d preflight checks.  All bits must be green before open() is allowed. */
enum {
    XEGFX_M3D_CHECK_KAPI     = 1u << 0,  /* v1 table readable, flags known */
    XEGFX_M3D_CHECK_CONTRACT = 1u << 1,  /* nub contract: vbt-child-devices */
    XEGFX_M3D_CHECK_TIMING   = 1u << 2,  /* KAPI getTiming() READY          */
    XEGFX_M3D_CHECK_MODE     = 1u << 3,  /* measured mode -> mode table     */
    XEGFX_M3D_CHECK_SCANOUT  = 1u << 4,  /* core scanout extension present  */
    XEGFX_M3D_CHECK_DT       = 1u << 5,  /* (c) 13 words -> detailed timing */
    XEGFX_M3D_CHECKS_ALL     = (1u << 6) - 1u
};

/* PlatformFunction selectors (accel <-> FB contract, m3d-accel 2026-10-28). */
#define XEGFX_M3D_FN_SCANOUT_INFO  "xgGetScanoutInfo"  /* accel -> FB      */
#define XEGFX_M3D_FN_FLIP_PRESENT  "xgFlipPresent"     /* FB -> accel      */
#define XEGFX_M3D_FN_VBLANK_COUNT  "xgVblankCount"     /* FB -> accel      */
#define XEGFX_M3D_FN_VBLANK_TIME   "xgVblankTime"      /* FB -> accel      */

/* accel -> FB: xgGetScanoutInfo, version 2, exactly 80 bytes.  m3d-accel
 * consumes this struct byte for byte; it only looks at flags bit 0. */
#define XEGFX_M3D_SCANOUT_INFO_VERSION  2u
#define XEGFX_M3D_SCANOUT_INFO_BYTES    80u
#define XEGFX_M3D_FORMAT_ARGB           0x42475241u /* 'ARGB' = BGRA 32bpp */

enum {
    XEGFX_M3D_SCANOUT_READY    = 1u << 0, /* allocated backing importable; no plane-write permission */
    XEGFX_M3D_SCANOUT_ZEROCOPY = 1u << 1, /* ggtt field is usable          */
    XEGFX_M3D_SCANOUT_FBOWNS   = 1u << 2, /* FB owns scanout backing        */
    XEGFX_M3D_SCANOUT_GGTT     = 1u << 3, /* ggtt field is valid           */
};

struct XeGfxScanoutInfo {
    uint32_t version;          /*  0 in: 2                              */
    uint32_t size;             /*  4 in: 80                             */
    uint64_t phys;             /*  8 host phys of allocated scanout     */
    uint64_t kva;              /* 16 kernel VA of that buffer, 0 = none */
    uint32_t pitch;            /* 24 bytes/row, 256 B aligned           */
    uint32_t width;            /* 28                                    */
    uint32_t height;           /* 32                                    */
    uint32_t format;           /* 36 0x42475241 or 0                    */
    uint32_t head;             /* 40 display head (4 for M3d)           */
    uint32_t flags;            /* 44 XEGFX_M3D_SCANOUT_*                */
    uint32_t activeScanout;    /* 48                                    */
    uint32_t scanoutCount;     /* 52                                    */
    uint64_t vblankCount;      /* 56 FB's own count for head            */
    uint64_t vblankNs;         /* 64 last observed vblank, ns, 0 = none */
    uint32_t ggtt;             /* 72 low 32 bits GGTT offset, 0 = none  */
    uint32_t reserved;         /* 76                                    */
};

/* FB -> accel: xgFlipPresent, version 2, exactly 80 bytes.  The layout is
 * identical to legacy v1; the accel side accepts version 1 or 2 (v2 is the
 * contract version, bit1 = NO_WAIT).  This stack always presents the whole
 * frame (region fields are validated but frame-sized). */
#define XEGFX_M3D_PRESENT_VERSION        2u
#define XEGFX_M3D_PRESENT_VERSION_LEGACY 1u  /* long-term accepted by accel */
#define XEGFX_M3D_PRESENT_BYTES          80u
#define XEGFX_M3D_PRESENT_WAIT_LATCH 0u      /* flags bit1: 0 = wait for latch */
#define XEGFX_M3D_PRESENT_NO_WAIT    (1u << 1)

struct XeGfxPresent {
    uint32_t version;          /*  0 = 2 (1 = legacy, accepted)         */
    uint32_t size;             /*  4 = 80                               */
    uint64_t phys;             /*  8 contiguous host phys of the frame  */
    uint64_t kva;              /* 16 caller mapping (debug only)        */
    uint32_t ggtt;             /* 24 256-aligned GGTT offset, 0 = none  */
    uint32_t pitch;            /* 28                                    */
    uint32_t width;            /* 32                                    */
    uint32_t height;           /* 36                                    */
    uint32_t format;           /* 40 0x42475241 or 0                    */
    uint32_t head;             /* 44 4 (0 => 4)                         */
    uint32_t regionX;          /* 48                                    */
    uint32_t regionY;          /* 52                                    */
    uint32_t regionW;          /* 56 0 => whole frame                   */
    uint32_t regionH;          /* 60                                    */
    uint64_t flipCount;        /* 64 caller monotonic counter           */
    uint32_t flags;            /* 72 XEGFX_M3D_PRESENT_*                */
    uint32_t reserved;         /* 76                                    */
};

#if defined(__cplusplus)
static_assert(sizeof(struct XeGfxScanoutInfo) == XEGFX_M3D_SCANOUT_INFO_BYTES,
              "XeGfxScanoutInfo must be the 80-byte accel ABI v2");
static_assert(offsetof(struct XeGfxScanoutInfo, phys) == 8,
              "XeGfxScanoutInfo.phys must be at offset 8");
static_assert(offsetof(struct XeGfxScanoutInfo, pitch) == 24,
              "XeGfxScanoutInfo.pitch must be at offset 24");
static_assert(offsetof(struct XeGfxScanoutInfo, flags) == 44,
              "XeGfxScanoutInfo.flags must be at offset 44");
static_assert(offsetof(struct XeGfxScanoutInfo, vblankCount) == 56,
              "XeGfxScanoutInfo.vblankCount must be at offset 56");
static_assert(offsetof(struct XeGfxScanoutInfo, ggtt) == 72,
              "XeGfxScanoutInfo.ggtt must be at offset 72");
static_assert(sizeof(struct XeGfxPresent) == XEGFX_M3D_PRESENT_BYTES,
              "XeGfxPresent must be the 80-byte accel ABI v1");
static_assert(offsetof(struct XeGfxPresent, phys) == 8,
              "XeGfxPresent.phys must be at offset 8");
static_assert(offsetof(struct XeGfxPresent, ggtt) == 24,
              "XeGfxPresent.ggtt must be at offset 24");
static_assert(offsetof(struct XeGfxPresent, flipCount) == 64,
              "XeGfxPresent.flipCount must be at offset 64");
#endif

class XeGfxBacklightHandler;   /* B1, XeGfxDisplay.cpp */

class XeGfxFramebuffer : public IOFramebuffer
{    OSDeclareDefaultStructors(XeGfxFramebuffer)

public:
    /* ---- IOService --------------------------------------------------- */
    /*
     * M2b: probe() is the -xedisp gate.  IOKit calls it before start(); with
     * the switch absent (the default) the personality matches nothing.
     */
    virtual IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
    virtual bool      start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void      stop(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void      free() APPLE_KEXT_OVERRIDE;

    /* ---- IOFramebuffer: lifecycle ------------------------------------ */
    virtual IOReturn  enableController() APPLE_KEXT_OVERRIDE;
    virtual IOReturn  open() APPLE_KEXT_OVERRIDE;
    virtual void      close() APPLE_KEXT_OVERRIDE;

    /* ---- IOFramebuffer: identity ------------------------------------- */
    virtual const char *getPixelFormats() APPLE_KEXT_OVERRIDE
                        { return IO32BitDirectPixels; }
    virtual IOItemCount  getConnectionCount() APPLE_KEXT_OVERRIDE
                        { return fInert ? 0 : 1; }
    virtual bool         isConsoleDevice() APPLE_KEXT_OVERRIDE
                        { return fInert ? false : fIsConsole; }

    /* ---- IOFramebuffer: mode table (six methods that must agree) ----- */
    virtual IOItemCount getDisplayModeCount() APPLE_KEXT_OVERRIDE;
    virtual IOReturn    getDisplayModes(IODisplayModeID *allDisplayModes) APPLE_KEXT_OVERRIDE;
    virtual IOReturn    getInformationForDisplayMode(IODisplayModeID mode,
                                                     IODisplayModeInformation *info) APPLE_KEXT_OVERRIDE;
    virtual IOReturn    getTimingInfoForDisplayMode(IODisplayModeID mode,
                                                    IOTimingInformation *info) APPLE_KEXT_OVERRIDE;
    virtual IOReturn    getCurrentDisplayMode(IODisplayModeID *mode,
                                              IOIndex *depth) APPLE_KEXT_OVERRIDE;
    virtual IOReturn    setDisplayMode(IODisplayModeID mode, IOIndex depth) APPLE_KEXT_OVERRIDE;

    virtual IOReturn    validateDetailedTiming(void *description,
                                               IOByteCount descripSize) APPLE_KEXT_OVERRIDE;
    virtual IOReturn    setDetailedTimings(OSArray *array) APPLE_KEXT_OVERRIDE;

    virtual IOReturn    getPixelInformation(IODisplayModeID mode, IOIndex depth,
                                            IOPixelAperture aperture,
                                            IOPixelInformation *info) APPLE_KEXT_OVERRIDE;
    virtual UInt64      getPixelFormatsForDisplayMode(IODisplayModeID,
                                                      IOIndex) APPLE_KEXT_OVERRIDE
                        { return 0; }
    virtual IOReturn    setStartupDisplayMode(IODisplayModeID, IOIndex) APPLE_KEXT_OVERRIDE
                        { return kIOReturnSuccess; }
    virtual IOReturn    getStartupDisplayMode(IODisplayModeID *mode,
                                              IOIndex *depth) APPLE_KEXT_OVERRIDE
                        { return getCurrentDisplayMode(mode, depth); }

    /* M3d: last-vblank timestamp/delta, needed by CoreDisplay's frame pacing.
     * IOFramebuffer has no getVBLCount(); the count is exposed through the
     * xgGetScanoutInfo PlatformFunction instead. */
    virtual void        getVBLTime(AbsoluteTime *time,
                                   AbsoluteTime *delta) APPLE_KEXT_OVERRIDE;

    /* ---- IOFramebuffer: memory --------------------------------------- */
    virtual IODeviceMemory *getApertureRange(IOPixelAperture aperture) APPLE_KEXT_OVERRIDE;
    virtual IODeviceMemory *getVRAMRange() APPLE_KEXT_OVERRIDE;
    virtual IOReturn setApertureEnable(IOPixelAperture, IOOptionBits) APPLE_KEXT_OVERRIDE
                        { return kIOReturnSuccess; }

    /* ---- IOFramebuffer: attributes / DDC / interrupts ---------------- */
    virtual IOReturn getAttribute(IOSelect attribute, uintptr_t *value) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setAttribute(IOSelect attribute, uintptr_t value) APPLE_KEXT_OVERRIDE;
    virtual IOReturn getAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
                                               uintptr_t *value) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
                                               uintptr_t value) APPLE_KEXT_OVERRIDE;
    virtual IOReturn getAppleSense(IOIndex connectIndex, UInt32 *senseType,
                                   UInt32 *primary, UInt32 *extended,
                                   UInt32 *displayType) APPLE_KEXT_OVERRIDE;
    virtual bool     hasDDCConnect(IOIndex) APPLE_KEXT_OVERRIDE
                        { return fConn.edidSize >= 128; }
    virtual IOReturn getDDCBlock(IOIndex connectIndex, UInt32 blockNumber,
                                 IOSelect blockType, IOOptionBits options,
                                 UInt8 *data, IOByteCount *length) APPLE_KEXT_OVERRIDE;

    virtual IOReturn registerForInterruptType(IOSelect type,
                                              IOFBInterruptProc proc,
                                              OSObject *target, void *ref,
                                              void **interruptRef) APPLE_KEXT_OVERRIDE;
    virtual IOReturn unregisterInterrupt(void *interruptRef) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setInterruptState(void *interruptRef, UInt32 state) APPLE_KEXT_OVERRIDE;

    /* ---- IOFramebuffer: cursor / gamma / power ----------------------- */
    virtual IOReturn setCursorImage(void *cursorImage) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setCursorState(SInt32 x, SInt32 y, bool visible) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setGammaTable(UInt32 channelCount, UInt32 dataCount,
                                   UInt32 dataWidth, void *data) APPLE_KEXT_OVERRIDE;
    /*
     * IOFramebuffer's primary reserved entry point is the 5-argument form; the
     * 4-argument form above is the compatibility path.  Overriding only one of
     * them hides the other overload (-Woverloaded-virtual) and lets CoreDisplay
     * miss the table, so both are routed to the same worker.
     */
    virtual IOReturn setGammaTable(UInt32 channelCount, UInt32 dataCount,
                                   UInt32 dataWidth, void *data,
                                   bool syncToVBL) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setCLUTWithEntries(IOColorEntry *colors, UInt32 index,
                                        UInt32 numColors,
                                        IOOptionBits options) APPLE_KEXT_OVERRIDE
                        { return kIOReturnSuccess; }
    virtual IOReturn setPowerState(unsigned long ordinal, IOService *device) APPLE_KEXT_OVERRIDE;

    /* ---- The second public API: XeGfxAccel -> FB ---------------------
     * NVRMFB exposes the same set of symbols to NVAccel.  Flip lives here,
     * not in IOFramebuffer.
     */
    virtual IOReturn callPlatformFunction(const OSSymbol *functionName,
                                          bool waitForFunction,
                                          void *param1, void *param2,
                                          void *param3, void *param4) APPLE_KEXT_OVERRIDE;

protected:
    /* ---- phases ------------------------------------------------------ */
    bool     readKapi(IOService *provider);      /* M1 */
    bool     kapiInit();                         /* M1 + head claim */
    bool     publishAgdcTiming();
    void     loadNubEdid(IOService *provider);   /* panel EDID the core read over AUX */
    bool     buildModeTable();                   /* M2 */
    bool     buildMeasuredModeTable(const void *bytes, uint32_t length); /* M3a */
    void     buildK3Modes();                     /* K3: fixed modes at other refresh rates */
    void     publishScalerCaps();                /* A1: IOFBScalerInfo + IOFBTimingRange */
    bool     scaledOk(const IODetailedTimingInformationV2 *dt, const char **why) const;  /* A1 */
    int      dtIndex(IODisplayModeID mode) const;  /* A1: fDT index of a scaled mode, or -1 */
    IOReturn setDesktop(uint32_t w, uint32_t h);   /* A1: KAPI setPlane + scanout view */
    int      s3Index(IODisplayModeID mode) const;  /* S3 mode id -> fMeasuredModes index, or -1 */
    void     logMeasuredModes() const;                                  /* M3a */
    static bool parseNvk5Timing(const uint32_t *words, uint32_t wordCount,
                                struct XeGfxKmsTiming *out,
                                uint64_t *pixelClockHz, uint32_t *refreshMHz,
                                const char **why);                      /* M3a */

    /* ---- modes ------------------------------------------------------- */
    IOReturn timingFromIo(const IODetailedTimingInformationV2 *dt,
                          struct XeGfxKmsTiming *out) const;
    void     timingToIo(const struct XeGfxKmsTiming *t,
                        IODetailedTimingInformationV2 *dt) const;
    int      findModeByModeId(IODisplayModeID modeId) const;
    int      findModeByTiming(const IODetailedTimingInformationV2 *dt) const;

    /* ---- vblank ------------------------------------------------------ */
    static void vblankBridge(void *arg, uint64_t count, uint32_t deltaUs);
    void        vblankDeliver(uint64_t count, uint32_t deltaUs);
    void        startVblankFallback();
    void        stopVblankFallback();
    static void vblankTimerAction(OSObject *owner, IOTimerEventSource *src);

    /* ---- cursor ------------------------------------------------------ */
    void     cursorApplyLocked();

    /* ---- helpers ----------------------------------------------------- */
    uint64_t nowUs() const;
    void     logModes() const;

    /* ---- M3c S3: same-mode claim (no register writes, no scanout) ---- */
    bool     startS3(IOService *provider);
    uint32_t s3RunChecks(const char *who);       /* logs the first failure   */
    bool     s3BuildAcceptedTiming();            /* (c) 13 words -> fAcceptedDt */
    bool     timingMatchesAccepted(const IODetailedTimingInformationV2 *dt) const;
    void     publishS3Timing();
    void     startS3Watchdog();
    void     stopS3Watchdog();
    void     s3WatchdogFire();
    static void s3WatchdogCall(thread_call_param_t p0, thread_call_param_t p1);
    /* M3d: XeGfxL0Ready/IOFBDependent* must land after start() returned;
     * a short thread_call keeps accel from calling back into a half-started
     * framebuffer (the 2026-10-27 getAttributeExt panic). */
    void     s3L0PublishArm();
    void     s3L0PublishFire();
    static void s3L0PublishCall(thread_call_param_t p0, thread_call_param_t p1);
    void     logS3Gates(const char *who) const;

    /* ---- M3d: real scanout/aperture/flip/vblank ----------------------
     * Every one of these returns early unless m3dGateEnabled() is true, so
     * they are dead code on a machine that has not opted into phase 2 +
     * trial + expose.  None of them writes a display register directly:
     * the only MMIO writer is the accel kext, through xgFlipPresent.
     */
    bool     m3dGateEnabled() const;
    uint32_t m3dRunChecks(const char *who);
    bool     m3dAllocScanout();
    bool     m3dApertureReady() const;
    void     m3dPublishScanoutProps();
    void     m3dPublishReady();
    bool     m3dEnsurePresented();
    IOReturn m3dPresent(struct XeGfxPresent *req);
    bool     m3dFindAccel(bool wait);
    bool     m3dTimingAgrees(const struct XeGfxKmsTiming *measured,
                             uint32_t refreshMHz) const;
    bool     m3dAccelVblank(uint64_t *count, uint64_t *ns);
    void     m3dLogState(const char *who) const;
    void     m3dStartPresentRetry();
    void     m3dStopPresentRetry();
    void     m3dPresentRetryFire();
    static void m3dPresentRetryCall(thread_call_param_t p0,
                                    thread_call_param_t p1);
    void     m3dStartVblankSource();
    void     m3dStopVblankSource();

    /* ---- KAPI -------------------------------------------------------- */
    const struct XeGfxKmsKapi *fKapi = nullptr;
    void                      *fCtx  = nullptr;

    /* ---- identity ---------------------------------------------------- */
    uint32_t  fIndex      = 0;      /* "fb-index"                          */
    uint32_t  fHandle     = 0;      /* connector handle                    */
    uint32_t  fHead       = 0;      /* pipe, from headMask                 */
    bool      fOwnsHead   = false;
    bool      fIsConsole  = false;
    bool      fReady      = false;  /* XeGfxFbReady published              */

    /* ---- M2b read-only state ----------------------------------------- */
    bool      fSkeleton  = false;   /* nub has no xegfx-kms-kapi yet       */
    bool      fInert     = false;   /* M3c S2: registered but inert FB     */
    bool      fFbStarted = false;   /* IOFramebuffer::start() ran          */
    bool      fReadOnly  = true;    /* no KMS calls, no display registers  */
    bool      fLoggedM2b = false;   /* one-shot read-only log              */

    /* ---- M3c S3: same-mode claim state -------------------------------
     * S3 never programs a register and never allocates a scanout.  It only
     * validates that the measured raster agrees with the KAPI mode and then
     * publishes exactly one detailed timing for it.
     */
    bool      fS3           = false;  /* phase==2 && trial==1 && !fail     */
    bool      fS3Fail       = false;  /* xegfx-disp-fail != 0              */
    bool      fFbModeReady  = false;  /* XeGfxFbModeReady published        */
    uint32_t  fS3Checks     = 0;      /* XEGFX_S3_CHECK_* bitmask          */
    uint32_t  fPhase        = 0;
    uint32_t  fTrial        = 0;
    uint32_t  fExpose       = 0;      /* logged, deliberately ignored <M3d */
    IODetailedTimingInformationV2 fAcceptedDt = {};
    uint32_t  fDtRejects    = 0;      /* setDetailedTimings drops          */
    uint32_t  fValidateRejects = 0;   /* validateDetailedTiming drops      */
    uint32_t  fOpenCount    = 0;      /* successful opens (watchdog signal) */
    uint32_t  fOpenDenied   = 0;      /* refused attempts, diagnosis only  */
    bool      fOpened       = false;
    bool      fWatchdogFired = false;
    thread_call_t fWatchdog = nullptr;

    /* ---- M3a measured mode table (never registered, never enabled) ---- */
    struct XeGfxMeasuredMode fMeasuredModes[XEGFX_DISPLAY_MAX_MEASURED_MODES];
    uint32_t  fMeasuredModeCount = 0;
    bool      fMeasuredOk = false;  /* words parsed and converted          */
    /*
     * K3: fMeasuredModes[2..] are the panel's other fixed modes (same raster, another refresh rate) that the core can
     * commit through KAPI setMode; [1] stays the inactive HiDPI candidate.  fKmsIdx maps an fMeasuredModes index to
     * the core's fixed-mode index, fS3Cur is the index the pipe runs now.
     */
    bool      fK3 = false;
    uint32_t  fK3Count = 0;
    uint32_t  fS3Cur = 0;
    uint32_t  fKmsIdx[XEGFX_DISPLAY_MAX_MEASURED_MODES] = {};
    /*
     * A1: scaled desktops (docs/03-PLAN §30.19).  CoreDisplay proposes them from IOFBScalerInfo as detailed timings
     * whose raster is one of the base modes above and whose horizontalScaled x verticalScaled is the desktop; they
     * are kept here with the base they scale (fDtBase, an fMeasuredModes index).  The scanout is allocated for the
     * largest desktop and fScanout[0].width/height/pitch describe the desktop on the plane now (fDeskW x fDeskH).
     * s3Index() returns XEGFX_A1_DT_INDEX + i for fDT[i].
     */
#define XEGFX_A1_MAX_DT     64
#define XEGFX_A1_DT_INDEX   100
    bool      fScalerOk = false;
    /* B1: the panel's brightness, a "backlight" IODisplayParameterHandler over KAPI backlight (docs/03-PLAN §30.26) */
    XeGfxBacklightHandler *fBacklight = nullptr;
    void      publishBacklight();
    uint32_t  fNDT = 0;
    IODetailedTimingInformationV2 fDT[XEGFX_A1_MAX_DT];
    uint8_t   fDtBase[XEGFX_A1_MAX_DT] = {};
    uint32_t  fDeskW = 0, fDeskH = 0;
    uint64_t  fScanoutCapBytes = 0;   /* the allocation, for the largest desktop */
    /*
     * K4 (§32.20): the core booted into the mode last set (NVRAM xegfx-disp-bootmode).  fS3Cur and the desktop start
     * as it committed them; fBootDeskPending until the detailed timing of that scaled desktop is known and reported as
     * the current mode, so WindowServer's first setDisplayMode finds the panel already there.
     */
    bool      fBootDeskPending = false;
    uint32_t  fBootSavedMHz = 0, fBootSavedW = 0, fBootSavedH = 0;
    void      k4ApplyBootState();
    void      k4SaveMode(uint32_t mhz, uint32_t w, uint32_t h);

    /* ---- connector snapshot (M1) ------------------------------------- */
    struct XeGfxKmsConnector fConn;

    /* ---- mode table (M2) --------------------------------------------- */
    IODisplayModeID *fIoModeIds  = nullptr;
    IOItemCount      fIoModeCount = 0;
    uint32_t        *fIoCfgModeIdx = nullptr;   /* io mode -> fConn.modes[] */
    IODisplayModeID  fCurrentId  = 1;
    uint32_t         fCurrentIdx = 0;
    bool             fModeSet    = false;

    /* ---- scanout (M3) ------------------------------------------------ */
    struct XeGfxKmsScanoutInfo fScanout[XEGFX_KMS_MAX_SCANOUT];
    uint32_t         fScanoutCount = 0;
    uint32_t         fActiveScanout = 0;
    IODeviceMemory  *fAperture = nullptr;
    IOPhysicalAddress fAperturePhys = 0;
    IOByteCount      fApertureLen  = 0;

    /* ---- vblank ------------------------------------------------------ */
    IOFBInterruptProc fVblProc   = nullptr;
    OSObject         *fVblTarget = nullptr;
    void             *fVblRef    = nullptr;
    void             *fVblCookie = nullptr;
    bool              fVblHW     = false;
    IOTimerEventSource *fVblTimer = nullptr;
    IOWorkLoop       *fWorkLoop  = nullptr;
    uint64_t          fLastVblUs = 0;
    /* E3: hardware vblank -- a kernel thread sleeps in the core's waitVblank (i915's vblank interrupt) and delivers
     * each vblank, instead of the free-running IOTimer.  fVblLock serialises delivery with (un)registration. */
    IOLock           *fVblLock       = nullptr;
    volatile bool     fVblThreadRun  = false;   /* the thread keeps running while true         */
    volatile bool     fVblThreadLive = false;   /* the thread has not exited yet               */
    volatile bool     fVblThreadOn   = false;   /* a client wants vblanks (setInterruptState)  */
    uint64_t          fVblHwWaits    = 0;       /* vblanks delivered from i915                 */
    uint64_t          fVblHwMisses   = 0;       /* waits that failed; a synthetic tick instead */
    uint64_t          fVblPropUs     = 0;       /* last XeGfxVblankCount publication           */
    static void vblankThreadMain(void *arg, wait_result_t wr);
    bool        startVblankThread();
    void        stopVblankThread();
    void        vblankPublishCount(uint64_t now);
    uint32_t          fRefreshHz = 60;

    /* ---- M3d: real FB service state (all gated by fM3d) -------------- */
    bool      fM3d             = false; /* expose arm chosen at startS3    */
    bool      fM3dCandidate    = false; /* preflight green + scanout mapped */
    bool      fM3dReady        = false; /* enableController completed      */
    uint32_t  fM3dChecks       = 0;     /* XEGFX_M3D_CHECK_*               */
    uint32_t  fKapiFlags       = 0;     /* v1 table flags word             */
    bool      fKapiTimingReady = false; /* getTiming() returned READY      */
    uint32_t  fKapiTimingWords[XEGFX_NVK5_TIMING_WORDS] = {};
    uint32_t  fChildDevices    = 0;     /* nub "vbt-child-devices"         */
    uint32_t  fStarts          = 0;     /* NVRAM counted starts            */
    uint32_t  fScanoutHandle   = 0;     /* KAPI opaque scanout handle      */
    uint32_t  fScanoutGGTT     = 0;     /* low 32 bits GGTT, 0 = none      */
    bool      fScanoutMapped   = false;
    bool      fPresented       = false; /* accel confirmed PLANE_SURF      */
    uint32_t  fPresentFails    = 0;
    uint64_t  fFlipCount       = 0;     /* monotonic, passed to accel      */
    bool      fPresentGaveUp   = false;
    thread_call_t fPresentTimer = nullptr;
    thread_call_t fL0Timer     = nullptr;
    uint32_t  fVblSource       = 0;     /* 0 none, 1 timer, 2 accel        */
    uint64_t  fVblCount        = 0;     /* 64-bit, survives 32-bit wrap    */
    uint64_t  fVblNs           = 0;     /* floor ns of last vblank         */
    uint64_t  fAccelVblCount   = 0;
    OSObject *fAccelObj        = nullptr; /* retained accel IOService      */
    IOReturn  fLastPresentRc   = kIOReturnNotReady;
    uint32_t  fM3dPresentBad   = 0;     /* hard string: accel rejected     */
    char      fM3dWhy[64]      = {0};   /* last check failure, diagnosis   */
    bool      fGammaLogged     = false; /* one-shot M3d log-once markers   */
    bool      fCursorLogged    = false;

    /* ---- display gate: serialises every KAPI call --------------------
     * Never take this from an interrupt context.  Never hold it while
     * waiting for a flip that itself needs it.
     */
    IOLock  *fGate = nullptr;

    /* ---- cursor state (cheap, separate from the gate) ----------------- */
    IOLock  *fCursorLock = nullptr;
    SInt32   fCurX = 0, fCurY = 0;
    bool     fCurVisible = false;
    uint32_t fCurSurface = 0xFFFFFFFFu;
    uint16_t fCurW = 0, fCurH = 0;
    /* C1: the core's hardware cursor (KAPI reserved[6]/[7]); NVRAM xegfx-disp-swcursor=1 keeps the software one */
    bool     hwCursorOn();
    uint32_t fCurImages = 0, fCurRefused = 0;
    int      fSwCursor = -1;

    /* ---- gamma ------------------------------------------------------- */
    void    *fGamma = nullptr;
    uint32_t fGammaLen = 0;

    /* ---- power ------------------------------------------------------- */
    bool fPowerOn  = true;
    bool fSysSlept = false;

    /* ---- graphics switches ------------------------------------------- */
    bool fBootInDT     = false;   /* firmware raster taken over as mode 0?  */
    bool fDisableMode  = false;   /* -xedispdt=0                            */
    bool fNoAccel      = false;   /* -xedispaccel=0                         */
};

/*
 * The class IOKit actually instantiates (Info.plist IOClass).
 *
 * CoreDisplay picks the GPU vendor of every IOFramebuffer from its C++ class
 * name (GetGPUVendorForFramebufferService: "Intel" -> IG, "AMD", "NVDA",
 * "AppleParavirt"; anything else is an unknown vendor).  With only unknown
 * vendors UseIOPresentment() silently clears useIOP, the display falls back
 * to the legacy CGXDisplayDevice surface and CompositorSW, which asserts in
 * CA::OGL::Context::push_surface on the first flattened layer.  The class
 * name must therefore contain "Intel".
 *
 * Not "XeGfxIntelFramebuffer": that name belongs to the XeGfxIntelFB
 * placeholder, and XeGfxAccel checks metaCast("XeGfxIntelFramebuffer") first.
 * All behaviour stays in XeGfxFramebuffer; serviceMatching / metaCast /
 * ioreg -c lookups of "XeGfxFramebuffer" keep matching this subclass.
 */
class XeGfxIntelKMSFramebuffer : public XeGfxFramebuffer
{    OSDeclareDefaultStructors(XeGfxIntelKMSFramebuffer)
};

#endif /* _XEGFXDISPLAY_H_ */
