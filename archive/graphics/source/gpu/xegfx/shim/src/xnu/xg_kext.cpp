/*
 * XeGfx kext entry. The module loads idle on IOResources and does nothing until a stage is asked for:
 *   sysctl debug.xegfx_stage=N runs every stage up to N that has not run yet (1 shim, 2 read-only GPU probe,
 *   3 bus and DMA, 4 drm/i915 SYSINITs and a GT health check, 5 one request on every engine,
 *   6 every engine writes a dword the CPU reads back, 7 set nmxgpu-claimed for the Metal accelerator). debug.xegfx_env sets tunables for the stages (overrides boot-args).
 * Two ways in. Manually, `kmutil load -b org.watchthelight.XeGfx` publishes XeGfxArm itself. At boot, XeGfxFBClaim
 * publishes XeGfxBootArm before XeGfxArm; _start then adds the display tunable and starts a kernel thread that runs
 * stages 1..7 after 2 s (M4a), so IOKit matching returns at once and the adapter's framebuffer probe runs after
 * XeGfxDisplayNub exists. There is no PCI personality: the shim finds the GPU in the registry.
 * On the manual path a stage that runs longer than debug.xegfx_wdog seconds panics on purpose, so a stall becomes a
 * panic report and an automatic reboot. The boot path clears that watchdog, stops at the first failing stage and
 * publishes XeGfxCoreReady only after stage 7; debug.xegfx_boot reports what it did.
 * <wtl> project: xegfx | author: watchthelight </wtl>
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IONVRAM.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <architecture/i386/pio.h>
#include <kern/thread_call.h>
#include <kern/thread.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <mach/kmod.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include <string.h>
#include "xg_i915.h"
#include "xg_fd.h"
#include "xg_vmap.h"

extern "C" {
void xg_start_stage(int stage);
int xg_drm_attached(void);
int xg_pci_xnu_start(void *iopcidevice);
void xg_newbus_dump(void);
void xg_unimpl(const char *name) __attribute__((__noreturn__));
int xg_display_disp_level(void);
unsigned int xg_display_vbt_get(const void **data);
int xg_display_children_get(const void **data, unsigned int *child_size);
const void *xg_display_kms_kapi(void);
unsigned int xg_display_kms_kapi_size(void);
unsigned int xg_display_kms_kapi_version(void);
void xg_display_set_measured_timing(const void *data, unsigned int len);
void xg_display_set_boot_mode(unsigned int refresh_mhz, unsigned int width, unsigned int height);
void xg_display_set_external_dp(int enabled);
int xg_display_external_dp(void);
int xg_display_aux_probe(void);
unsigned int xg_display_edid_get(const void **data);
unsigned int xg_display_dpcd_get(const void **data);
kern_return_t _start(kmod_info_t *ki, void *d);
kern_return_t _stop(kmod_info_t *ki, void *d);
KMOD_EXPLICIT_DECL(org.watchthelight.XeGfx, "0.0.32", _start, _stop)
char xg_env[512];
int xg_progress;	/* last breadcrumb a running stage reached; the guard journals it */
extern unsigned int xg_pci_irqs;
extern "C" unsigned int xg_pci_handled;
extern "C" {
struct xg_irqoff {
	volatile unsigned long long t0; volatile unsigned long pc; volatile int what;
	/* §37.15: the 11-03 13:46 hang named a CPU in a spin section that never answered the NMI after it.  The rest
	 * names what each CPU waits on: its thread and callers, the spin mutex it is waiting for and that lock's owner,
	 * and an MMIO read still in flight (a read the GPU never completes stalls the CPU even for an NMI). */
	volatile void *td; volatile unsigned long caller[3];
	volatile const char *lock; volatile unsigned long owner;
	volatile unsigned long long mmio_t0; volatile unsigned long mmio_addr; volatile int mmio_who;	/* 1 i915 readl/readq, 2 accel */
};
extern struct xg_irqoff xg_irqoff[64];
unsigned long xg_mmio_regs_base(void);
void xg_mmio_begin_who(const volatile void *addr, int who);
void xg_mmio_end(void);
extern int xg_spin_panic;
}
static thread_call_t gIrqoffCall;
static volatile UInt32 gIrqoffArmed;
static vm_address_t gKextBase;

/*
 * N-287..N-289 died in XNU's rendezvous timeout: one CPU kept interrupts off for half a second and the report never
 * says which. This runs on a CPU that still answers, every 20 ms, and names the slot first (100 ms).
 */
/* §37.15: every CPU with interrupts off, a spin mutex it waits for, or an MMIO read in flight for over 1 ms -- with
 * the owner's CPU when another slot holds that thread, and callers as XeGfx offsets (kernel addresses as they are) */
static void
xg_irqoff_report(char *out, size_t len, uint64_t now)
{
	size_t n = 0;
	const unsigned long base = (unsigned long)gKextBase, regs = xg_mmio_regs_base();
	out[0] = 0;
#define XG_REP(...) do { if (n < len) { int w = snprintf(out + n, len - n, __VA_ARGS__); if (w > 0) n += (size_t)w; } } while (0)
#define XG_OFF(a) ((a) >= base && (a) < base + 0x400000ul ? (a) - base : (a))
	XG_REP("\nxegfx cpus (regs %#lx):", regs);
	for (int c = 0; c < 64; c++) {
		const uint64_t t0 = xg_irqoff[c].t0, m0 = xg_irqoff[c].mmio_t0;
		uint64_t ns = 0, mns = 0;
		if (t0 && t0 <= now) absolutetime_to_nanoseconds(now - t0, &ns);
		if (m0 && m0 <= now) absolutetime_to_nanoseconds(now - m0, &mns);
		if (!ns && mns < 1000000ull) continue;
		XG_REP("\n cpu %d:", c);
		if (ns) {
			XG_REP(" irqoff %llums %s pc+%#lx td %p by %#lx<%#lx<%#lx", ns / 1000000, xg_irqoff[c].what == 2 ? "filter" : "spin",
			    XG_OFF(xg_irqoff[c].pc), (void *)xg_irqoff[c].td, XG_OFF(xg_irqoff[c].caller[0]),
			    XG_OFF(xg_irqoff[c].caller[1]), XG_OFF(xg_irqoff[c].caller[2]));
			const char *lk = (const char *)xg_irqoff[c].lock;
			const unsigned long ow = xg_irqoff[c].owner;
			if (lk) {
				int oc = -1;
				for (int d = 0; d < 64; d++) if (ow && (unsigned long)xg_irqoff[d].td == ow) { oc = d; break; }
				XG_REP(" waits '%s' owner %#lx (cpu %d)", lk, ow, oc);
			}
		}
		if (mns >= 1000000ull) {
			const unsigned long a = xg_irqoff[c].mmio_addr;
			XG_REP(" mmio %s %#lx (+%#lx) %llums", xg_irqoff[c].mmio_who == 2 ? "accel" : "i915", a,
			    regs && a >= regs && a < regs + 0x1000000ul ? a - regs : 0ul, mns / 1000000);
		}
	}
#undef XG_OFF
#undef XG_REP
}

static void
xg_irqoff_check(thread_call_param_t, thread_call_param_t)
{
	uint64_t now = mach_absolute_time(), ns;

	for (int c = 0; c < 64; c++) {
		uint64_t t0 = xg_irqoff[c].t0;
		if (t0 == 0 || t0 > now)
			continue;
		absolutetime_to_nanoseconds(now - t0, &ns);
		if (ns > 100000000ull && xg_spin_panic) {
			static char rep[1400];
			xg_irqoff_report(rep, sizeof rep, now);
			panic("xegfx: cpu %d has had interrupts off for %llu ms in %s, pc %#lx (XeGfx base %#lx, offset %#lx)%s",
			    c, ns / 1000000, xg_irqoff[c].what == 2 ? "the interrupt filter" : "a spin section",
			    xg_irqoff[c].pc, (unsigned long)gKextBase, xg_irqoff[c].pc - (unsigned long)gKextBase, rep);
		}
	}
	uint64_t dl;
	clock_interval_to_deadline(20, NSEC_PER_MSEC, &dl);
	thread_call_enter_delayed(gIrqoffCall, dl);
}
extern int xg_spin_panic;
extern unsigned long long xg_spin_max_us;
extern unsigned int xg_spin_long;
/* the firmware's console image, when pipe A scans it out at probe; i915's stolen allocator starts after it */
unsigned long long xg_boot_fb_phys, xg_boot_fb_size;
}

class XeGfxResident : public IOService {
	OSDeclareDefaultStructors(XeGfxResident)
public:
	bool start(IOService *provider) override;
	using IOService::callPlatformFunction;
	IOReturn callPlatformFunction(const OSSymbol *name, bool wait, void *p1, void *p2, void *p3, void *p4) override;
};

static XeGfxResident *gResident;
static int gStage;	/* highest stage done */
static int gRunning;	/* stage in progress, 0 when idle */
static int gResult[8];	/* per stage: 0 not run, 1 pass, negative errno-style failure */
static int gWdog = 60;
static thread_call_t gWdogCall;
static thread_call_t gHoldCall;	/* armed around userspace checks: a GPU deadlock there panics too */
static int gHold;
/*
 * NVRAM trace (I-104: a stage 4 hang left no log at all): with xegfx.nvram_trace=1 in debug.xegfx_env, every 250 ms
 * while a stage runs, "xegfx-step" gets the stage, the breadcrumb and the newest console line, synced at once.
 */
static thread_call_t gTraceCall;
static bool gTraceOn;
static char gLast[600];	/* the newest console lines, oldest dropped first */
static unsigned gLastSeq, gTraceSeq;
static char gAccel[96];	/* XeGfxAccel's breadcrumb (xgTrace), NVRAM xegfx-accel */
static unsigned gAccelSeq, gAccelTraceSeq;
static int gTraceProgress;
static void xg_trace_write(void);

/* strstr is not in the public kext KPIs */
/* strstr is not in the public kext KPIs */
static const char *
strstr_kpi(const char *h, const char *n)
{
	size_t l = strlen(n);

	for (; *h; h++)
		if (strncmp(h, n, l) == 0)
			return h;
	return nullptr;
}

static bool
xg_env_has(const char *kv)
{
	size_t n = strlen(kv);

	for (const char *p = xg_env; *p; p++)
		if (strncmp(p, kv, n) == 0)
			return true;
	return false;
}
static IOLock *gStageLock;
static uint64_t gSurfPte;
static bool gPipeLit;	/* pipe A was scanning out at probe time (Hybrid); in Discrete mode the NVIDIA GPU has the panel */
static char gLog[256 * 1024];	/* last 256 KiB of the shim's console output, for sysctl debug.xegfx_log */
static size_t gLogLen;
static IOSimpleLock *gLogLock;
static IOLock *gSnapLock;	/* GGTT entry behind PLANE_SURF_1_A at probe time, 0 if unknown */

bool
XeGfxResident::start(IOService *provider)
{
	if (!IOService::start(provider))
		return false;
	gResident = this;
	registerService();
	return true;
}

/* "xgDrmIoctl" (minor, cmd, user arg, int *errno): DRM for WindowServer, through XeGfxAccel's user client */
extern "C" int xg_display_kms_joiner_info(unsigned int *active, unsigned int *delta, unsigned int *gen);
extern "C" int xg_display_kms_plane_layout(int want, int *cur);
extern "C" int xg_display_kms_plane_layout2(int want, int *cur, unsigned int ccs_off, unsigned int ccs_pitch);
struct xg_pipe_colour;
extern "C" int xg_display_kms_colour(const struct xg_pipe_colour *c, unsigned int *us_out);	/* §37.28 */
extern "C" int xg_surf_ticket(void *md, void *task, unsigned int *ticket, unsigned long long *size);	/* §37.17 */

IOReturn
XeGfxResident::callPlatformFunction(const OSSymbol *name, bool wait, void *p1, void *p2, void *p3, void *p4)
{
	if (name && name->isEqualTo("xgDrmIoctl") && p4) {
		*(int *)p4 = xg_fd_remote_ioctl((int)(uintptr_t)p1, (unsigned long)p2, (unsigned long long)(uintptr_t)p3);
		return kIOReturnSuccess;
	}
	/* "xgFlipInit" (surface bytes, slots wanted, u64 *ggtt out, unsigned *count out) / "xgFlipMap" (slot,
	 * phys page array, npages, surface bytes) / "xgFlipUnmap" (slot, surface bytes): zero-copy scanout.  The
	 * accelerator writes PLANE_SURF itself; this side owns the GGTT slots and the PTEs. */
	if (name && name->isEqualTo("xgFlipInit") && p3 && p4) {
		int err = xg_flip_init((unsigned long long)(uintptr_t)p1, (unsigned int)(uintptr_t)p2,
		    (unsigned long long *)p3, (unsigned int *)p4);
		return (err == 0 ? kIOReturnSuccess : kIOReturnError);
	}
	if (name && name->isEqualTo("xgFlipMap") && p2) {
		int err = xg_flip_map((unsigned int)(uintptr_t)p1, (unsigned long long)(uintptr_t)p4,
		    (const unsigned long long *)p2, (unsigned int)(uintptr_t)p3);
		return (err == 0 ? kIOReturnSuccess : kIOReturnError);
	}
	if (name && name->isEqualTo("xgFlipUnmap")) {
		int err = xg_flip_unmap((unsigned int)(uintptr_t)p1, (unsigned long long)(uintptr_t)p2);
		return (err == 0 ? kIOReturnSuccess : kIOReturnError);
	}
	/* "xgScanoutPhys" (token, u64 *phys out, capacity, u64 out[2] = { pages, bytes }): §37.14 -- the pages of a GEM object
	 * the plugin pinned for scanout, one per 4 KiB as xgFlipMap takes them; kIOReturnNoSpace (with out set) if too few. */
	if (name && name->isEqualTo("xgScanoutPhys") && p4) {
		unsigned int n = 0;
		unsigned long long bytes = 0;
		int err = xg_scanout_bo_phys((unsigned int)(uintptr_t)p1, (unsigned long long *)p2, (unsigned int)(uintptr_t)p3,
		    &n, &bytes);
		((unsigned long long *)p4)[0] = n;
		((unsigned long long *)p4)[1] = bytes;
		return (err == 0 ? kIOReturnSuccess : err == -ENOSPC ? kIOReturnNoSpace : kIOReturnNotFound);
	}
	/* "xgSurfTicket" (IOMemoryDescriptor *md, task_t task, unsigned *ticket out, u64 *size out): §37.17 -- the accelerator
	 * hands over an IOSurface's descriptor; the plugin announces the ticket with the userptr import it makes next
	 * (SURF_EXPECT, xg_i915_map.c), which then gets the surface's memory.  kIOReturnBadArgument / NoMemory on failure. */
	if (name && name->isEqualTo("xgSurfTicket") && p1 && p2 && p3 && p4) {
		int err = xg_surf_ticket(p1, p2, (unsigned int *)p3, (unsigned long long *)p4);
		return (err == 0 ? kIOReturnSuccess : err == -ENOMEM ? kIOReturnNoMemory : kIOReturnBadArgument);
	}
	/* "xgMmioHooks" (void **begin out, void **end out): §37.15 -- begin (addr, who) / end () record a register read in
	 * flight on this CPU's watchdog slot; the accelerator wraps its own display register reads in them (who 2). */
	if (name && name->isEqualTo("xgMmioHooks") && p1 && p2) {
		*(void **)p1 = (void *)xg_mmio_begin_who;
		*(void **)p2 = (void *)xg_mmio_end;
		return (kIOReturnSuccess);
	}
	/* "xgJoinerInfo" (unsigned *active, unsigned *pipe-B PLANE_SURF delta, unsigned *gen): J1 -- while pipe A is joined
	 * with pipe B, every PLANE_SURF_1_A write needs PLANE_SURF_1_B = the same frame + delta (xg_display_hook.c). */
	if (name && name->isEqualTo("xgJoinerInfo") && p1 && p2) {
		int err = xg_display_kms_joiner_info((unsigned int *)p1, (unsigned int *)p2, (unsigned int *)p3);
		return (err == 0 ? kIOReturnSuccess : kIOReturnNotReady);
	}
	/* "xgPlaneLayout" (int want, int *cur out): §32.16 -- the primary plane scans Tile4 (1) or linear (0) frames; want
	 * < 0 only reads it.  A change re-commits the placeholder (plane-only) and re-reads the joiner delta. */
	if (name && name->isEqualTo("xgPlaneLayout") && p2) {
		int err = xg_display_kms_plane_layout((int)(intptr_t)p1, (int *)p2);
		return (err == 0 ? kIOReturnSuccess : err == -EAGAIN ? kIOReturnNotReady : kIOReturnError);
	}
	/* "xgPipeColour" (const struct xg_pipe_colour *, unsigned *us out, int *result out): §37.28 -- the colour state of
	 * WindowServer's display transactions committed to the pipe's degamma LUT, CSC and gamma LUT (xg_display_hook.c).
	 * Thread context, sleeps (an atomic commit). */
	if (name && name->isEqualTo("xgPipeColour") && p1 && p3) {
		*(int *)p3 = xg_display_kms_colour((const struct xg_pipe_colour *)p1, (unsigned int *)p2);
		return (kIOReturnSuccess);
	}
	/* "xgPlaneLayout2" (int want, int *cur out, u32 ccs offset, u32 ccs pitch): §37.14 -- as xgPlaneLayout, plus 2 =
	 * Tile4 with render compression (MTL RC CCS), the CCS at that offset and pitch from each frame's start. */
	if (name && name->isEqualTo("xgPlaneLayout2") && p2) {
		int err = xg_display_kms_plane_layout2((int)(intptr_t)p1, (int *)p2, (unsigned int)(uintptr_t)p3,
		    (unsigned int)(uintptr_t)p4);
		return (err == 0 ? kIOReturnSuccess : err == -EAGAIN ? kIOReturnNotReady : kIOReturnError);
	}
	/* "xgFlipQuery" (int *idle out): 1 when the GPU finished everything it was given (0 busy, -ENODEV only when
	 * no i915 device is bound); the accelerator's deferred flip worker polls this before it arms a frame. */
	if (name && name->isEqualTo("xgFlipQuery") && p1) {
		*(int *)p1 = xg_i915_idle();
		return kIOReturnSuccess;
	}
	/*
	 * §32.14 per-frame flip ordering: "xgFlipSnapshot" (void **out): the last request of every active timeline, taken
	 * when the compositor's transaction arrives (no sleep).  "xgFlipSnapWait" (snap, timeout ms, int *result,
	 * unsigned *fences): 1 all done, 0 timeout, -1 overflowed -- wait for idle instead (thread context, sleeps).
	 * "xgFlipSnapFree" (snap).
	 */
	if (name && name->isEqualTo("xgFlipSnapshot") && p1) {
		*(void **)p1 = xg_i915_flip_snapshot();
		return (*(void **)p1 != nullptr ? kIOReturnSuccess : kIOReturnNoMemory);
	}
	if (name && name->isEqualTo("xgFlipSnapWait") && p1 && p3) {
		*(int *)p3 = xg_i915_flip_snap_wait(p1, (unsigned int)(uintptr_t)p2, (unsigned int *)p4);
		return kIOReturnSuccess;
	}
	/* "xgGtMin" (int set_mhz or 0 to read, int *min out, int *max out): §37.29 -- XeGfxAccel raises the GPU clock floor
	 * while compositor frames miss their vblank and puts it back after.  Thread context, sleeps (an SLPC request). */
	if (name && name->isEqualTo("xgGtMin") && p2) {
		const int r = xg_i915_gt_min_mhz((int)(intptr_t)p1);
		*(int *)p2 = r;
		if (p3)
			*(int *)p3 = xg_i915_gt_max_mhz();
		return (r < 0 ? kIOReturnError : kIOReturnSuccess);
	}
	if (name && name->isEqualTo("xgFlipSnapFree")) {
		xg_i915_flip_snap_free(p1);
		return kIOReturnSuccess;
	}
	/* "xgTrace" (const char *line): XeGfxAccel's state, stored with the console trace (thread context only) */
	if (name && name->isEqualTo("xgTrace") && p1 && gLogLock) {
		IOInterruptState is = IOSimpleLockLockDisableInterrupt(gLogLock);
		strlcpy(gAccel, (const char *)p1, sizeof(gAccel));
		gAccelSeq++;
		IOSimpleLockUnlockEnableInterrupt(gLogLock, is);
		return kIOReturnSuccess;
	}
	return IOService::callPlatformFunction(name, wait, p1, p2, p3, p4);
}
OSDefineMetaClassAndStructors(XeGfxResident, IOService)

/* M2a: the display nub the IOFramebuffer driver (XeGfxDisplay.kext) matches on.  The core
 * publishes it once, after the VBT parser ran, and keeps no opinion about its clients. */
class XeGfxDisplayNub : public IOService {
	OSDeclareDefaultStructors(XeGfxDisplayNub)
public:
	bool start(IOService *provider) override;
	void stop(IOService *provider) override;
};

OSDefineMetaClassAndStructors(XeGfxDisplayNub, IOService)

bool
XeGfxDisplayNub::start(IOService *provider)
{
	if (!IOService::start(provider))
		return false;
	registerService();
	return true;
}

void
XeGfxDisplayNub::stop(IOService *provider)
{
	IOService::stop(provider);
}

static XeGfxDisplayNub *gDisplayNub;

static void
xg_wdog_fire(thread_call_param_t, thread_call_param_t)
{
	/* the panic report is the only record a stall leaves, so it carries the end of the driver's log (N-224) */
	static char tail[3072];
	size_t n = MIN(gLogLen, sizeof(tail) - 1);

	if (!gRunning)
		return;
	memcpy(tail, gLog + gLogLen - n, n);	/* no lock: the stalled thread may hold it; a torn line is fine here */
	tail[n] = 0;
	panic("XeGfx: stage %d stalled for %d s at progress %d\n--- last driver log ---\n%s", gRunning, gWdog,
	    xg_progress, tail);
}

/* ---- stage 2: read-only look at the iGPU and the PCH watchdog, published as XeGfxProbe ---- */

static void
xg_put(OSDictionary *d, const char *k, uint64_t v, int bits = 32)
{
	OSNumber *n = OSNumber::withNumber(v, bits);

	if (n) {
		d->setObject(k, n);
		n->release();
	}
}

static IOPCIDevice *
xg_find_pci(uint16_t vendor, uint8_t cls, uint8_t sub)
{
	IOPCIDevice *found = nullptr;
	OSDictionary *m = IOService::serviceMatching("IOPCIDevice");
	OSIterator *it = m ? IOService::getMatchingServices(m) : nullptr;

	OSSafeReleaseNULL(m);
	if (!it)
		return nullptr;
	while (OSObject *o = it->getNextObject()) {
		IOPCIDevice *p = OSDynamicCast(IOPCIDevice, o);
		if (p && p->configRead16(kIOPCIConfigVendorID) == vendor &&
		    p->configRead8(kIOPCIConfigClassCode + 2) == cls && p->configRead8(kIOPCIConfigClassCode + 1) == sub) {
			found = p;
			found->retain();
			break;
		}
	}
	it->release();
	return found;
}

/*
 * ---- M4a timing fallback: a panel timing source that does not need the IntelFB kext -------------------------
 *
 * The display nub publishes the NVK5 13-word panel timing as "measured-panel-timing" and the adapter
 * (XeGfxDisplay) builds its read-only IOFramebuffer mode table from it.  Until M4a that record could only come
 * from XeGfxIntelFB, which measures the live pipe while macOS's own framebuffer driver owns the display.  With
 * M4a the core claims the PCI display class itself, IntelFB never starts, the record is missing and the adapter
 * ends up with zero modes.
 *
 * Priority (measured first, so a boot with IntelFB running behaves exactly as before):
 *   1 "measured"  XeGfxIntelFramebuffer's XeGfxMeasuredPanelTiming, unchanged;
 *   2 "edid-dtd"  the panel EDID's base detailed timing, compiled in below: this panel's real raster;
 *   3 "vbt-dtd"   the firmware VBT's LFP DTD (BDB 40 LFP_OPTIONS, 41 LFP_DATA_PTRS, 42 LFP_DATA), parsed the
 *                 way i915's parse_lfp_data()/fill_detail_timing_data() parse it.  Last resort only: on this
 *                 unit the VBT carries a generic mode list, not the 2560x1600 panel (docs/20);
 *   4 none:       no "measured-panel-timing" property at all and an IOLog warning (as before).
 * All three produce the same 13 words (nvrm-agdc-k5.h / nvrm-agdc.cpp nvk5_timing_bad):
 *   0 tag 0x4b350001, 1 pixel clock Hz, 2 h visible, 3 h sync start, 4 h sync end, 5 h total,
 *   6 v visible, 7 v sync start, 8 v sync end, 9 v total, 10 refresh mHz, 11 hsync positive, 12 vsync positive.
 * Nothing here touches a register or sleeps; it only decodes data that is already in memory.
 */
#define XG_NVK5_TIMING_TAG	0x4b350001u
#define XG_NVK5_TIMING_WORDS	13

/*
 * The panel EDID's base detailed timing: 18 bytes (offset 54) of the 384-byte blob the AGDC kext ships in
 * kexts/NVRMAGDC/xe/Info.plist (IOKitPersonalities -> XeGfxAGDCLoader -> XeGfxPanelEDID: BOE NE160QDM-NZL,
 * week 38/2024).  AGDC matches its measured registers against this same blob (nvrm-agdc.cpp:
 * xeFirmwareEDIDTiming), so a timing synthesized from it is the raster macOS/IntelFB would have measured.
 *   277,440,000 Hz, 2560x1600 visible, hblank 160 (48 front / 32 sync / 80 back),
 *   vblank 100 (6 front / 6 sync / 88 back), hsync positive, vsync negative.
 */
static const uint8_t gPanelEdidDtd[18] = {
	0x60, 0x6c, 0x00, 0xa0, 0xa0, 0x40, 0x64, 0x60, 0x30,
	0x20, 0x66, 0x00, 0x59, 0xd7, 0x10, 0x00, 0x00, 0x1a,
};

static uint32_t gTimingWords[XG_NVK5_TIMING_WORDS];
static int gTimingWordsValid;
static int gTimingMeasured;	/* 1: the record came from XeGfxIntelFramebuffer */
static int gTimingEdid;		/* 1: a fallback built it from gPanelEdidDtd */
static int gTimingVbt;		/* 1: a fallback built it from the VBT LFP DTD */
static char gTimingSource[16] = "none";
static char gTimingNote[128];	/* why the fallback declined, or where the record came from */
static char gTimingChild[64];	/* the VBT internal connector the fallback identified */

static inline uint16_t
xg_le16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t
xg_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*
 * One 18-byte detailed timing descriptor -> the 13 NVK5 words.  edid_std picks the flag byte encoding:
 * EDID 1.4 (bit 1 hsync positive, bit 2 vsync positive) for the panel EDID, and struct bdb_edid_dtd's
 * bitfields as i915 reads them (bits 6/5, deps/drm-kmod/.../intel_vbt_defs.h) for a VBT LFP DTD.  The raster
 * fields have the same packing in both formats, which is why fill_detail_timing_data() reads them like this.
 * Mirrors xegfxPanelTiming() (xe/panel-timing.h): truncating integer refresh, every field must fit 16 bits.
 */
static int
xg_nvk5_from_dtd(const uint8_t *d, int edid_std, uint32_t *w)
{
	uint64_t pclk = (uint64_t)xg_le16(d) * 10000ull;	/* the descriptor stores the clock in 10 kHz */
	uint32_t hactive = (uint32_t)d[2] | ((uint32_t)(d[4] >> 4) << 8);
	uint32_t hblank = (uint32_t)d[3] | ((uint32_t)(d[4] & 0x0f) << 8);
	uint32_t vactive = (uint32_t)d[5] | ((uint32_t)(d[7] >> 4) << 8);
	uint32_t vblank = (uint32_t)d[6] | ((uint32_t)(d[7] & 0x0f) << 8);
	uint32_t hoff = (uint32_t)d[8] | ((((uint32_t)d[11] >> 6) & 0x03) << 8);
	uint32_t hpw = (uint32_t)d[9] | ((((uint32_t)d[11] >> 4) & 0x03) << 8);
	uint32_t voff = (uint32_t)(d[10] >> 4) | ((((uint32_t)d[11] >> 2) & 0x03) << 4);
	uint32_t vpw = (uint32_t)(d[10] & 0x0f) | (((uint32_t)d[11] & 0x03) << 4);
	uint32_t htot = hactive + hblank, vtot = vactive + vblank;
	uint32_t mhz;
	int hpos, vpos;

	if (edid_std) {
		hpos = (d[17] >> 1) & 1;
		vpos = (d[17] >> 2) & 1;
	} else {
		/* bdb_edid_dtd: rsvd1:3, digital:2, vsync_positive:1, hsync_positive:1, non_interlaced:1 */
		hpos = (d[17] >> 6) & 1;
		vpos = (d[17] >> 5) & 1;
	}
	if (pclk == 0 || pclk > 0xffffffffull || hactive == 0 || vactive == 0)
		return -EINVAL;
	if (hoff == 0 || hpw == 0 || voff == 0 || vpw == 0)
		return -EINVAL;	/* a panel raster always has a front porch and a sync pulse */
	if (htot > 0xffffu || vtot > 0xffffu || htot == hactive || vtot == vactive)
		return -EINVAL;
	if (hactive + hoff + hpw > htot || vactive + voff + vpw > vtot)
		return -EINVAL;
	mhz = (uint32_t)(pclk * 1000ull / ((uint64_t)htot * vtot));
	if (mhz < 1000 || mhz > 1000000)
		return -EINVAL;

	w[0] = XG_NVK5_TIMING_TAG;
	w[1] = (uint32_t)pclk;
	w[2] = hactive;
	w[3] = hactive + hoff;
	w[4] = hactive + hoff + hpw;
	w[5] = htot;
	w[6] = vactive;
	w[7] = vactive + voff;
	w[8] = vactive + voff + vpw;
	w[9] = vtot;
	w[10] = mhz;
	w[11] = (uint32_t)hpos;
	w[12] = (uint32_t)vpos;
	return 0;
}

/* the first captured VBT child that is an internal connector (eDP/DSI): DEVICE_TYPE_INTERNAL_CONNECTOR = 1<<12 */
static int
xg_vbt_internal_child(uint16_t *handle, uint16_t *type, uint8_t *port)
{
	const void *data = NULL;
	unsigned int size = 0;
	int count = xg_display_children_get(&data, &size);

	for (int i = 0; i < count && data != NULL && size >= 40; i++) {
		const uint8_t *c = (const uint8_t *)data + (size_t)i * size;

		if ((xg_le16(c + 2) & 0x1000) == 0)
			continue;
		*handle = xg_le16(c);
		*type = xg_le16(c + 2);
		*port = c[16];
		return i;
	}
	return -1;
}

/* find one BDB block; the walk is i915 find_raw_section(): bdb header, then id(1)+size(2), no overrun allowed */
static const uint8_t *
xg_vbt_bdb_block(const uint8_t *bdb, uint32_t bdb_len, uint8_t want, uint32_t *block_len)
{
	uint32_t off;

	if (bdb_len < 0x16 || memcmp(bdb, "BIOS_DATA_BLOCK", 15) != 0)
		return NULL;
	off = xg_le16(bdb + 0x12);	/* bdb header_size */
	if (off < 0x16 || off > bdb_len)
		return NULL;
	while (off + 3 <= bdb_len) {
		uint32_t size = xg_le16(bdb + off + 1);

		if (off + 3 + size > bdb_len)
			return NULL;	/* a block that runs past the BDB: i915's walk stops here too */
		if (bdb[off] == want) {
			*block_len = size;
			return bdb + off + 3;
		}
		off += 3 + size;
	}
	return NULL;
}

/*
 * "vbt-dtd": the LFP panel DTD out of the VBT.  BDB 40 LFP_OPTIONS byte 0 is panel_type; BDB 42 LFP_DATA
 * holds the panel entries; BDB 41 LFP_DATA_PTRS maps panel_type to its dvo_timing entry.  This VBT has no
 * block 41, so the table i915's generate_lfp_data_ptrs() builds is used instead: 38-byte fp_timing, then the
 * 18-byte bdb_edid_dtd, then the 10-byte pnp id, 66 bytes per entry, dvo_timing at +38.
 */
static int
xg_timing_from_vbt(uint32_t *w)
{
	const void *vbt_data = NULL;
	unsigned int vbt_len = xg_display_vbt_get(&vbt_data);
	const uint8_t *vbt = (const uint8_t *)vbt_data, *bdb, *opts, *data, *ptrs;
	uint32_t bdb_off, bdb_len, opts_len = 0, data_len = 0, ptrs_len = 0, panel_type, off;

	if (vbt == NULL || vbt_len < 0x30 || memcmp(vbt, "$VBT", 4) != 0) {
		snprintf(gTimingNote, sizeof(gTimingNote), "no VBT captured");
		return -ENOENT;
	}
	bdb_off = xg_le32(vbt + 0x1c);
	if (bdb_off < 0x30 || bdb_off + 0x16 > vbt_len) {
		snprintf(gTimingNote, sizeof(gTimingNote), "VBT bdb_offset %u out of range", bdb_off);
		return -EINVAL;
	}
	bdb = vbt + bdb_off;
	bdb_len = xg_le16(bdb + 0x14);
	if (bdb_len > vbt_len - bdb_off)
		bdb_len = vbt_len - bdb_off;
	opts = xg_vbt_bdb_block(bdb, bdb_len, 40, &opts_len);	/* BDB_LFP_OPTIONS */
	data = xg_vbt_bdb_block(bdb, bdb_len, 42, &data_len);	/* BDB_LFP_DATA */
	ptrs = xg_vbt_bdb_block(bdb, bdb_len, 41, &ptrs_len);	/* BDB_LFP_DATA_PTRS */
	if (opts == NULL || opts_len < 1 || data == NULL) {
		snprintf(gTimingNote, sizeof(gTimingNote), "VBT has no LFP options/data block");
		return -ENOENT;
	}
	panel_type = opts[0];
	if (panel_type == 0xff || panel_type > 0x0f) {
		snprintf(gTimingNote, sizeof(gTimingNote), "VBT panel_type %u invalid", panel_type);
		return -EINVAL;
	}
	if (ptrs != NULL && ptrs_len >= 1 && panel_type < ptrs[0])
		off = xg_le16(ptrs + 1 + panel_type * 9 + 3);	/* lfp_data_ptr[].dvo_timing.offset */
	else
		off = panel_type * 66u + 38u;			/* generate_lfp_data_ptrs() layout */
	if (off + 18 > data_len) {
		snprintf(gTimingNote, sizeof(gTimingNote),
		    "VBT panel_type %u dvo offset %u past LFP_DATA (%u B)", panel_type, off, data_len);
		return -EINVAL;
	}
	if (xg_nvk5_from_dtd(data + off, 0, w) != 0) {
		snprintf(gTimingNote, sizeof(gTimingNote), "VBT panel_type %u DTD rejected", panel_type);
		return -EINVAL;
	}
	snprintf(gTimingNote, sizeof(gTimingNote), "VBT LFP panel_type %u", panel_type);
	return 0;
}

/* fill the globals and hand the nub the 52-byte record: measured, edid-dtd and vbt-dtd all come through here */
static void
xg_timing_store(XeGfxDisplayNub *nub, const uint32_t *w, const char *source)
{
	memcpy(gTimingWords, w, sizeof(gTimingWords));
	gTimingWordsValid = 1;
	strlcpy(gTimingSource, source, sizeof(gTimingSource));
	if (OSData *d = OSData::withBytes(w, sizeof(gTimingWords))) {
		nub->setProperty("measured-panel-timing", d);
		d->release();
	}
	xg_display_set_measured_timing(w, sizeof(gTimingWords));
}

/* the fallback itself; called only when XeGfxIntelFramebuffer did not hand over a measured record */
static void
xg_timing_fallback(XeGfxDisplayNub *nub)
{
	uint32_t w[XG_NVK5_TIMING_WORDS];
	if (xg_display_external_dp()) {
		strlcpy(gTimingSource, "kms-pending", sizeof(gTimingSource));
		strlcpy(gTimingNote, "external DP waits for committed EDID timing; no embedded panel fallback", sizeof(gTimingNote));
		return;
	}
	uint16_t handle = 0, type = 0;
	uint8_t port = 0;
	int child = xg_vbt_internal_child(&handle, &type, &port);

	if (child >= 0)
		snprintf(gTimingChild, sizeof(gTimingChild), "internal child %d handle 0x%04x type 0x%04x port %u",
		    child, handle, type, port);
	else
		snprintf(gTimingChild, sizeof(gTimingChild), "internal child none");

	if (xg_nvk5_from_dtd(gPanelEdidDtd, 1, w) == 0) {
		gTimingEdid = 1;
		xg_timing_store(nub, w, "edid-dtd");
		snprintf(gTimingNote, sizeof(gTimingNote),
		    "embedded panel EDID DTD (BOE NE160QDM-NZL), IntelFB not running");
		IOLog("XeGfx: timing fallback: edid-dtd pclk %u Hz %ux%u ht %u vt %u hs %u..%u vs %u..%u refresh %u mHz\n",
		    w[1], w[2], w[6], w[5], w[9], w[3], w[4], w[7], w[8], w[10]);
		return;
	}
	if (xg_timing_from_vbt(w) == 0) {
		gTimingVbt = 1;
		xg_timing_store(nub, w, "vbt-dtd");
		IOLog("XeGfx: timing fallback: vbt-dtd pclk %u Hz %ux%u ht %u vt %u refresh %u mHz (%s)\n",
		    w[1], w[2], w[6], w[5], w[9], w[10], gTimingNote);
		return;
	}
	strlcpy(gTimingSource, "none", sizeof(gTimingSource));
	IOLog("XeGfx: timing fallback: no panel timing (%s); adapter will have no modes\n", gTimingNote);
}

/*
 * M2a: publish one display nub per GPU.  Properties are what the VBT parser found so far;
 * M2/M3 grow this into the NVKMS-style kapi table.  Only called from the display overlay
 * when disp_level >= 2, so nothing here runs on a normal boot.
 */
static bool xg_nvram_flag(const char *key);

extern "C" void
xg_display_nub_publish(int child_devices, int bdb_blocks)
{
	IOPCIDevice *gpu;
	XeGfxDisplayNub *nub;

	if (gDisplayNub)
		return;
	gpu = xg_find_pci(0x8086, 0x03, 0x00);
	if (!gpu)
		return;
	nub = new XeGfxDisplayNub;
	if (!nub) {
		gpu->release();
		return;
	}
	if (!nub->init()) {
		nub->release();
		gpu->release();
		return;
	}
	nub->setProperty("xegfx-display-level", (unsigned long long)xg_display_disp_level(), 32);
	nub->setProperty("vbt-bdb-blocks", (unsigned long long)bdb_blocks, 32);
	nub->setProperty("vbt-child-devices", (unsigned long long)child_devices, 32);
	nub->setProperty("fb-index", (unsigned long long)0, 32);

	/*
	 * M2c: hand the adapter kext the raw data it needs to build its own view of
	 * the panel before any display register is touched: the whole VBT image,
	 * the raw child_device_config records (fixed stride), and the measured
	 * panel timing snapshot that XeGfxIntelFB already publishes.
	 */
	{
		const void *vbt_data = NULL;
		unsigned int vbt_len = xg_display_vbt_get(&vbt_data);
		if (vbt_len > 0 && vbt_data != NULL) {
			if (OSData *d = OSData::withBytes(vbt_data, vbt_len)) {
				nub->setProperty("vbt-data", d);
				d->release();
			}
			nub->setProperty("vbt-size", (unsigned long long)vbt_len, 32);
		}
	}
	{
		const void *child_data = NULL;
		unsigned int child_size = 0;
		int child_count = xg_display_children_get(&child_data, &child_size);
		if (child_count > 0 && child_data != NULL && child_size > 0) {
			if (OSData *d = OSData::withBytes(child_data, (unsigned int)child_count * child_size)) {
				nub->setProperty("vbt-children", d);
				d->release();
			}
			nub->setProperty("vbt-child-size", (unsigned long long)child_size, 32);
		}
	}
	{
		int measured = 0, saw_measured = 0;
		OSDictionary *match = IOService::serviceMatching("XeGfxIntelFramebuffer");

		if (match != NULL) {
			OSIterator *it = IOService::getMatchingServices(match);
			if (it != NULL) {
				IOService *fb = OSDynamicCast(IOService, it->getNextObject());
				if (fb != NULL) {
					OSObject *timing = fb->copyProperty("XeGfxMeasuredPanelTiming");
					if (timing != NULL) {
						saw_measured = 1;
						nub->setProperty("measured-panel-timing", timing);
						if (OSData *td = OSDynamicCast(OSData, timing)) {
							xg_display_set_measured_timing(td->getBytesNoCopy(),
							    td->getLength());
							if (td->getLength() == sizeof(gTimingWords)) {
								memcpy(gTimingWords, td->getBytesNoCopy(),
								    sizeof(gTimingWords));
								gTimingWordsValid = 1;
								measured = 1;
								strlcpy(gTimingSource, "measured",
								    sizeof(gTimingSource));
								snprintf(gTimingNote, sizeof(gTimingNote),
								    "XeGfxIntelFramebuffer XeGfxMeasuredPanelTiming");
							}
						}
						timing->release();
					}
				}
				it->release();
			}
			match->release();
		}
		gTimingMeasured = measured;
		/* a measured record (even a malformed one) keeps the M3b behavior; only its absence is new */
		if (!saw_measured)
			xg_timing_fallback(nub);
		if (OSString *src = OSString::withCString(gTimingSource)) {
			nub->setProperty("XeGfxTimingSource", src);
			src->release();
		}
	}
	/*
	 * M3b: publish the read-only KMS KAPI v1.  The property is the address of
	 * the static function table (8 bytes, kernel memory owned by this kext, so
	 * it stays valid until the kext is unloaded).  The adapter casts the bytes
	 * back to a pointer, validates magic/version/size, then calls get_timing()
	 * and get_connector_info().  The version/size properties are a convenience
	 * for ioreg and for a future adapter that wants to check before mapping.
	 */
	{
		const void *kapi = xg_display_kms_kapi();
		if (kapi != NULL) {
			uint64_t kapi_ptr = (uint64_t)(uintptr_t)kapi;
			if (OSData *d = OSData::withBytes(&kapi_ptr, sizeof(kapi_ptr))) {
				nub->setProperty("xegfx-kms-kapi", d);
				d->release();
			}
			nub->setProperty("xegfx-kms-kapi-version",
			    (unsigned long long)xg_display_kms_kapi_version(), 32);
			nub->setProperty("xegfx-kms-kapi-size",
			    (unsigned long long)xg_display_kms_kapi_size(), 32);
		}
	}
	/*
	 * Panel identity, read from the panel itself over its AUX channel (as NVKMS gives NVRMFB its EDID): the raw
	 * EDID for the adapter's getDDCBlock and the DPCD capability regions for link/DSC/backlight decisions.
	 * NVRAM xegfx-disp-noaux skips the read; a failed read publishes nothing.
	 */
	if (xg_nvram_flag("xegfx-disp-noaux")) {
		IOLog("XeGfx: display nub: AUX read skipped (NVRAM xegfx-disp-noaux)\n");
	} else {
		const void *edid = NULL, *dpcd = NULL;
		unsigned int edid_len, dpcd_len;
		(void)xg_display_aux_probe();
		edid_len = xg_display_edid_get(&edid);
		dpcd_len = xg_display_dpcd_get(&dpcd);
		if (edid_len >= 128 && edid != NULL) {
			if (OSData *d = OSData::withBytes(edid, edid_len)) {
				nub->setProperty("xegfx-edid", d);
				d->release();
			}
		}
		if (dpcd_len > 0 && dpcd != NULL) {
			if (OSData *d = OSData::withBytes(dpcd, dpcd_len)) {
				nub->setProperty("xegfx-dpcd", d);
				d->release();
			}
		}
		IOLog("XeGfx: display nub: AUX EDID %u B, DPCD %u B\n", edid_len, dpcd_len);
	}
	if (xg_display_external_dp())
		nub->setProperty("xegfx-external-dp", kOSBooleanTrue);
	if (OSString *ver = OSString::withCString("0.5-aux-edid")) {
		nub->setProperty("xegfx-display-version", ver);
		ver->release();
	}
	if (!nub->attach(gpu)) {
		nub->release();
		gpu->release();
		return;
	}
	gpu->release();
	nub->registerService();
	gDisplayNub = nub;
	if (gResident)
		gResident->setProperty("XeGfxDisplayNub", nub);
}

static int
xg_probe(void)
{
	OSDictionary *d = OSDictionary::withCapacity(32);
	IOPCIDevice *gpu = xg_find_pci(0x8086, 0x03, 0x00), *smb = xg_find_pci(0x8086, 0x0c, 0x05);
	int rc = 1;

	if (!d)
		return -12;
	if (!gpu) {
		rc = -19;
		goto out;
	}
	xg_progress = 21;
	xg_put(d, "device-id", gpu->configRead16(kIOPCIConfigDeviceID), 16);
	xg_put(d, "revision", gpu->configRead8(kIOPCIConfigRevisionID), 8);
	xg_put(d, "subsystem", gpu->configRead32(kIOPCIConfigSubSystemVendorID));
	xg_put(d, "command", gpu->configRead16(kIOPCIConfigCommand), 16);
	xg_put(d, "ggc", gpu->configRead16(0x50), 16);
	for (int bar = 0; bar < 2; bar++) {
		IODeviceMemory *m = gpu->getDeviceMemoryWithRegister((UInt8)(kIOPCIConfigBaseAddress0 + bar * 8));
		char k[16];
		snprintf(k, sizeof(k), "bar%d-base", bar * 2);
		xg_put(d, k, m ? m->getPhysicalAddress() : 0, 64);
		snprintf(k, sizeof(k), "bar%d-size", bar * 2);
		xg_put(d, k, m ? m->getLength() : 0, 64);
	}
	/* register reads only, uncached BAR0 map; never BAR2 or stolen memory (Wa_22018444074) */
	if (gpu->configRead16(kIOPCIConfigCommand) & kIOPCICommandMemorySpace) {
		IOMemoryMap *mm = gpu->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0, kIOMapInhibitCache);
		if (mm) {
			volatile uint32_t *r = (volatile uint32_t *)mm->getVirtualAddress();
			static const struct { const char *k; uint32_t off; } regs[] = {
				{ "gmd-id-graphics", 0xd8c }, { "gmd-id-display", 0x510a0 },
				{ "dsm-base-lo", 0x1080c0 }, { "dsm-base-hi", 0x1080c4 },
				{ "pcode-stolen-access", 0x138914 },
				{ "transconf-a", 0x70008 }, { "pipe-frmcount-a", 0x70040 },
				{ "plane-ctl-1-a", 0x70180 }, { "plane-stride-1-a", 0x70188 }, { "plane-size-1-a", 0x70190 },
				{ "plane-surf-1-a", 0x7019c },
			};
			xg_progress = 22;
			for (auto &e : regs)
				xg_put(d, e.k, r[e.off / 4]);
			gPipeLit = (r[0x70008 / 4] & 0x80000000u) != 0;
			xg_put(d, "pipe-a-lit", gPipeLit, 8);
			if (mm->getLength() >= 16ull << 20) {
				uint32_t surf = r[0x7019c / 4] & ~0xfffu;
				volatile uint64_t *gtt = (volatile uint64_t *)(mm->getVirtualAddress() + (8ull << 20));
				gSurfPte = gtt[surf >> 12];
				xg_put(d, "ggtt-pte-surf", gSurfPte, 64);
				if (gPipeLit && (gSurfPte & 1)) {
					/* linear stride is in 64-byte units; PLANE_SIZE holds height - 1 in bits 31:16 */
					xg_boot_fb_phys = gSurfPte & 0x3ffffffff000ull;	/* GEN12_GGTT_PTE_ADDR_MASK */
					xg_boot_fb_size = (uint64_t)(r[0x70188 / 4] & 0x7ff) * 64 * ((r[0x70190 / 4] >> 16) + 1);
					xg_put(d, "boot-fb-size", xg_boot_fb_size, 64);
				}
			}
			mm->release();
		} else {
			rc = -5;
		}
	}
	xg_progress = 23;
	/* PCH TCO watchdog (SMBus function, TCOBASE at config 0x50, TCOCTL 0x54), I/O port reads only */
	if (smb) {
		uint16_t tcobase = smb->configRead16(0x50) & 0xffe0;
		uint32_t tcoctl = smb->configRead32(0x54);
		xg_put(d, "tco-base", tcobase, 16);
		xg_put(d, "tco-ctl", tcoctl);
		if (tcobase && (tcoctl & (1 << 8))) {
			xg_put(d, "tco-rld", inw(tcobase + 0x0), 16);
			xg_put(d, "tco1-sts", inw(tcobase + 0x4), 16);
			xg_put(d, "tco2-sts", inw(tcobase + 0x6), 16);
			xg_put(d, "tco1-cnt", inw(tcobase + 0x8), 16);
			xg_put(d, "tco2-cnt", inw(tcobase + 0xa), 16);
			xg_put(d, "tco-tmr", inw(tcobase + 0x12), 16);
		}
	}
out:
	xg_put(d, "result", (uint32_t)rc);
	if (gResident)
		gResident->setProperty("XeGfxProbe", d);
	d->release();
	OSSafeReleaseNULL(gpu);
	OSSafeReleaseNULL(smb);
	return rc;
}

/* ---- native display bring-up, step 0: the firmware's OpRegion and VBT --------------------------------
 *
 * The display driver takes its panel, connectors and backlight description from the VBT that the firmware leaves in
 * the ACPI OpRegion (or in the legacy mailbox #4 of it).  Nothing has looked at either since the GPU left the
 * firmware's hands, so before a single display register is written this surveys both read-only: config-space ASLS,
 * the OpRegion header/version/mailboxes, and the VBT (RVDA) header and BDB.  The panel keeps scanning out exactly
 * what it scanned before.  Write any value to `sysctl debug.xegfx_display` to run it, read the knob for the report.
 */
static char gDisp[1536];
static size_t gDispLen;

static void
xg_dispf(const char *fmt, ...)
{
	va_list ap;

	if (gDispLen >= sizeof(gDisp) - 1)
		return;
	va_start(ap, fmt);
	vsnprintf(gDisp + gDispLen, sizeof(gDisp) - gDispLen, fmt, ap);
	va_end(ap);
	gDispLen = strlen(gDisp);
}

/* map one physical range into the kernel map; returns the kernel virtual address of phys, or 0 */
static uint64_t
xg_map_ram(uint64_t phys, size_t len, IOMemoryMap **mapOut)
{
	uint64_t off = phys & 0xfffull;
	IOMemoryDescriptor *md;
	IOMemoryMap *map;

	md = IOMemoryDescriptor::withPhysicalAddress(phys & ~0xfffull, (IOByteCount)(off + len), kIODirectionIn);
	if (!md)
		return 0;
	map = md->map(kIOMapDefaultCache);
	md->release();
	if (!map)
		return 0;
	*mapOut = map;
	return (uint64_t)(uintptr_t)map->getVirtualAddress() + off;
}

/* a VBT as the display driver sees it: vbt_header, then bdb_header at bdb_offset; both signatures checked */
static bool
xg_vbt_report(uint64_t va, size_t avail, const char *source)
{
	uint16_t version, hdr, size, bdbVersion, bdbSize;
	uint32_t bdbOff;

	if (avail < 0x30 || memcmp((const void *)(uintptr_t)va, "$VBT", 4) != 0)
		return false;
	version = *(volatile uint16_t *)(uintptr_t)(va + 0x14);
	hdr = *(volatile uint16_t *)(uintptr_t)(va + 0x16);
	size = *(volatile uint16_t *)(uintptr_t)(va + 0x18);
	bdbOff = *(volatile uint32_t *)(uintptr_t)(va + 0x1c);
	if (size == 0 || size > avail || hdr > size || bdbOff + 0x16 > size)
		return false;
	if (memcmp((const void *)(uintptr_t)(va + bdbOff), "BIOS_DATA_BLOCK", 15) != 0)
		return false;
	bdbVersion = *(volatile uint16_t *)(uintptr_t)(va + bdbOff + 0x10);
	bdbSize = *(volatile uint16_t *)(uintptr_t)(va + bdbOff + 0x14);
	xg_dispf("vbt: found via %s: version %u.%u, header %u, size %u, bdb version %u, bdb size %u\n", source,
	    version >> 8, version & 0xff, hdr, size, bdbVersion, bdbSize);
	return true;
}

static int
xg_display_probe(void)
{
	IOPCIDevice *gpu = xg_find_pci(0x8086, 0x03, 0x00);
	IOMemoryMap *map = nullptr;
	uint64_t base;
	uint32_t asls;
	int rc = 1;

	gDispLen = 0;
	gDisp[0] = 0;
	if (!gpu)
		return -ENODEV;
	asls = gpu->configRead32(0xfc);
	xg_dispf("opregion: ASLS=0x%08x\n", asls);
	if (!asls) {
		xg_dispf("opregion: absent, so there is no firmware VBT; the display driver will need a synthetic "
			 "VBT built from the panel EDID instead\n");
		rc = 2;
		goto out;
	}
	base = xg_map_ram(asls & ~0xfffull, 0x2000, &map);
	if (!base) {
		xg_dispf("opregion: cannot map 0x%08x\n", asls & ~0xfffull);
		rc = -5;
		goto out;
	}
	{
		char sig[17];
		char vbios[17];
		uint32_t size, mboxes, pcon;
		uint8_t rev, min, maj;
		const uint64_t off = (uint64_t)(asls & 0xfffull);	/* ASLS need not be page aligned */
		const uint64_t hdr = base + off;
		bool found = false;

		memcpy(sig, (const void *)(uintptr_t)hdr, 16);
		sig[16] = 0;
		if (memcmp(sig, "IntelGraphicsMem", 16) != 0) {
			xg_dispf("opregion: signature mismatch at ASLS 0x%08x (asls+0x%llx: %02x %02x %02x %02x %02x %02x %02x %02x; page base: %02x %02x %02x %02x)\n",
			    asls, (unsigned long long)off, (uint8_t)sig[0], (uint8_t)sig[1], (uint8_t)sig[2], (uint8_t)sig[3],
			    (uint8_t)sig[4], (uint8_t)sig[5], (uint8_t)sig[6], (uint8_t)sig[7],
			    *(volatile uint8_t *)(uintptr_t)base, *(volatile uint8_t *)(uintptr_t)(base + 1),
			    *(volatile uint8_t *)(uintptr_t)(base + 2), *(volatile uint8_t *)(uintptr_t)(base + 3));
			rc = -6;
			goto out;
		}
		size = *(volatile uint32_t *)(uintptr_t)(hdr + 0x10);
		rev = *(volatile uint8_t *)(uintptr_t)(hdr + 0x15);
		min = *(volatile uint8_t *)(uintptr_t)(hdr + 0x16);
		maj = *(volatile uint8_t *)(uintptr_t)(hdr + 0x17);
		memcpy(vbios, (const void *)(uintptr_t)(hdr + 0x38), 16);
		vbios[16] = 0;
		mboxes = *(volatile uint32_t *)(uintptr_t)(hdr + 0x58);
		pcon = *(volatile uint32_t *)(uintptr_t)(hdr + 0x60);
		xg_dispf("opregion: ok: size %u, version %u.%u.%u, mboxes 0x%02x%s%s%s%s%s, pcon 0x%08x, vbios \"%s\"\n",
		    size, maj, min, rev, mboxes, (mboxes & (1u << 0)) ? " ACPI" : "",
		    (mboxes & (1u << 1)) ? " SWSCI" : "", (mboxes & (1u << 2)) ? " ASLE" : "",
		    (mboxes & (1u << 4)) ? " ASLE_EXT" : "", (mboxes & (1u << 5)) ? " BACKLIGHT" : "", pcon, vbios);
		if ((mboxes & (1u << 2)) != 0) {
			uint64_t rvda = *(volatile uint64_t *)(uintptr_t)(hdr + 0x300 + 0xba);
			uint32_t rvds = *(volatile uint32_t *)(uintptr_t)(hdr + 0x300 + 0xc2);

			xg_dispf("opregion: ASLE rvda=0x%llx rvds=%u\n", (unsigned long long)rvda, rvds);
			if (rvda && rvds >= 0x30) {
				uint64_t phys = rvda;
				IOMemoryMap *vmap = nullptr;
				uint64_t vbase;

				if (maj > 2 || (maj == 2 && min >= 1))
					phys = rvda + (uint64_t)asls;	/* 2.1+: offset from the OpRegion */
				vbase = xg_map_ram(phys, rvds, &vmap);
				if (vbase) {
					found = xg_vbt_report(vbase, rvds, "ASLE RVDA");
					vmap->release();
				} else {
					xg_dispf("vbt: cannot map RVDA 0x%llx (%u bytes)\n",
					    (unsigned long long)phys, rvds);
				}
			}
		}
		if (!found) {
			size_t avail = (size_t)(((mboxes & (1u << 4)) != 0 ? 0x1c00u : 0x2000u) - 0x400u);

			found = xg_vbt_report(base + 0x400, avail, "mailbox #4");
		}
		if (!found)
			xg_dispf("vbt: not found: no valid $VBT/BIOS_DATA_BLOCK in RVDA or mailbox #4\n");
	}
out:
	IOLog("XeGfx: display probe rc %d:\n%s", rc, gDisp);
	if (gResident)
		gResident->setProperty("XeGfxDisplayProbe", gDisp);
	if (map)
		map->release();
	OSSafeReleaseNULL(gpu);
	return rc;
}

static uint32_t gBootSurf;	/* PLANE_SURF_1_A after i915 ran: the boot surface's GGTT offset */
static uint32_t gKmsLevel;	/* NVRAM xegfx-disp-kms as applied this boot (claim boots only); 3 = K2 commits */

/* after i915 has run: the panel must still scan out the boot surface through the same GGTT entry, and pipe A must
 * still count frames; otherwise the desktop is black even though WindowServer looks fine */
static int
xg_display_check(void)
{
	OSDictionary *d = OSDictionary::withCapacity(8);
	IOPCIDevice *gpu = xg_find_pci(0x8086, 0x03, 0x00);
	IOMemoryMap *mm = gpu ? gpu->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0, kIOMapInhibitCache) : nullptr;
	int rc = -6;

	if (!gPipeLit && d) {
		/* nothing of ours on the panel to lose; say so instead of failing on a stopped frame counter */
		xg_put(d, "skipped-pipe-a-off", 1, 8);
		xg_put(d, "result", 1);
		if (gResident)
			gResident->setProperty("XeGfxPost", d);
		rc = 1;
	} else if (mm && d && mm->getLength() >= 16ull << 20) {
		volatile uint32_t *r = (volatile uint32_t *)mm->getVirtualAddress();
		uint32_t surf = r[0x7019c / 4] & ~0xfffu, f0 = r[0x70040 / 4];
		gBootSurf = surf;
		uint64_t pte = ((volatile uint64_t *)(mm->getVirtualAddress() + (8ull << 20)))[surf >> 12];
		IOSleep(100);
		uint32_t f1 = r[0x70040 / 4];
		xg_put(d, "plane-surf-1-a", surf);
		xg_put(d, "ggtt-pte-surf", pte, 64);
		xg_put(d, "frmcount-delta", f1 - f0);
		/*
		 * K2 (kms=3) commits a modeset of i915's own onto a placeholder framebuffer, so the plane no longer scans
		 * the boot surface by design: what must hold then is that pipe A runs again, which the frame counter shows.
		 */
		if (gKmsLevel >= 3) {
			xg_put(d, "k2-committed", 1, 8);
			rc = f1 == f0 ? -70 : 1;
		} else
			rc = (gSurfPte && pte != gSurfPte) ? -71 : (f1 == f0 ? -70 : 1);
		xg_put(d, "result", (uint32_t)rc);
		if (gResident)
			gResident->setProperty("XeGfxPost", d);
	}
	OSSafeReleaseNULL(d);
	OSSafeReleaseNULL(mm);
	OSSafeReleaseNULL(gpu);
	return rc;
}

/* i915 probe succeeds even when GuC loading failed and the GT was wedged; fail stage 4 on that instead */
static int
xg_gt_check(void)
{
	struct xg_i915_state st;
	OSDictionary *d = OSDictionary::withCapacity(10);
	int rc = xg_i915_state(&st) == 0 ? 1 : -19;

	if (rc == 1 && st.wedged)
		rc = -5;
	else if (rc == 1 && (st.guc_status != 10 || !st.guc_submission))
		rc = -62;	/* 10 is INTEL_UC_FIRMWARE_RUNNING */
	if (d) {
		xg_put(d, "gts", st.gts, 8);
		xg_put(d, "wedged", st.wedged, 8);
		xg_put(d, "guc-fw-status", (uint32_t)st.guc_status);
		xg_put(d, "guc-version", st.guc_ver);
		xg_put(d, "guc-submission", st.guc_submission, 8);
		xg_put(d, "huc-fw-status", (uint32_t)st.huc_status);
		xg_put(d, "gsc-fw-status", (uint32_t)st.gsc_status);
		xg_put(d, "engines", st.engines, 8);
		xg_put(d, "render-minor", (uint32_t)st.render_minor);
		xg_put(d, "result", (uint32_t)rc);
		if (gResident)
			gResident->setProperty("XeGfxGt", d);
		d->release();
	}
	return rc;
}

/*
 * stage 7: hand the GPU to the Metal accelerator (XeGfxAccel, NMXAccel built over XeGfx). Its personality matches
 * only an IOPCIDevice with nmxgpu-claimed, so nothing matched the live iGPU at boot; registerService re-runs matching
 * on the nub, which adds the accelerator in its own match category and leaves every existing client alone.
 */
static int
xg_accel_claim(void)
{
	IOPCIDevice *gpu = xg_find_pci(0x8086, 0x03, 0x00);

	if (gpu == nullptr)
		return -19;
	gpu->setProperty("nmxgpu-claimed", kOSBooleanTrue);
	gpu->registerService();
	gpu->release();
	return 1;
}

/* stage 5: a kernel request per engine proves GuC submission, the MSI path and the breadcrumb wake together */
static int
xg_exec_check(void)
{
	static struct xg_i915_exec ex;
	unsigned int irq0 = xg_pci_irqs;
	OSDictionary *d = OSDictionary::withCapacity(XG_I915_MAX_ENGINES + 4);
	int rc = xg_i915_exec(&ex, 2000) == 0 ? 1 : -19;

	if (rc == 1 && (ex.engines == 0 || ex.done != ex.engines))
		rc = -62;
	if (d) {
		for (int i = 0; i < ex.engines; i++) {
			char k[24];
			snprintf(k, sizeof(k), "%s-ns", ex.name[i]);
			xg_put(d, k, (uint64_t)ex.ns[i], 64);
			snprintf(k, sizeof(k), "%s-err", ex.name[i]);
			xg_put(d, k, (uint32_t)ex.err[i]);
		}
		xg_put(d, "engines", ex.engines, 8);
		xg_put(d, "done", ex.done, 8);
		xg_put(d, "irqs", xg_pci_irqs - irq0);
		xg_put(d, "result", (uint32_t)rc);
		if (gResident)
			gResident->setProperty("XeGfxExec", d);
		d->release();
	}
	return rc;
}

/* stage 6: the GPU writes memory the CPU then reads, through the GGTT, on every engine that can */
static int
xg_store_check(void)
{
	static struct xg_i915_store sd;
	OSDictionary *d = OSDictionary::withCapacity(2 * XG_I915_MAX_ENGINES + 3);
	int rc = xg_i915_store(&sd, 2000) == 0 ? 1 : -19;

	if (rc == 1 && (sd.engines == 0 || sd.ok != sd.engines))
		rc = -5;
	if (d) {
		for (int i = 0; i < sd.engines; i++) {
			char k[24];
			snprintf(k, sizeof(k), "%s-got", sd.name[i]);
			xg_put(d, k, sd.got[i]);
			snprintf(k, sizeof(k), "%s-err", sd.name[i]);
			xg_put(d, k, (uint32_t)sd.err[i]);
		}
		xg_put(d, "engines", (uint32_t)sd.engines, 8);
		xg_put(d, "ok", (uint32_t)sd.ok, 8);
		xg_put(d, "result", (uint32_t)rc);
		if (gResident)
			gResident->setProperty("XeGfxStore", d);
		d->release();
	}
	return rc;
}

static int
xg_run_stage(int s)
{
	int rc = 1;

	gRunning = s;
	xg_progress = s * 10;
	gTraceOn = xg_env_has("xegfx.nvram_trace=1");
	xg_spin_panic = !xg_env_has("xegfx.spin_panic=0");
	if (gTraceOn) {
		xg_trace_write();
		thread_call_enter(gTraceCall);
	}
	/* xegfx.irqoff_check=0 turns the checker off: N-290 passed with it, is its 20 ms wakeup the fix? */
	if (gIrqoffCall && !xg_env_has("xegfx.irqoff_check=0") && OSCompareAndSwap(0, 1, &gIrqoffArmed))
		xg_irqoff_check(nullptr, nullptr);
	if (gWdogCall && gWdog > 0) {
		uint64_t dl;
		clock_interval_to_deadline(gWdog, NSEC_PER_SEC, &dl);
		thread_call_enter_delayed(gWdogCall, dl);
	}
	if (s == 2) {
		rc = xg_probe();
	} else {
		xg_start_stage(s);
		if (s == 1 && xg_vm_vmap_selftest() != 0)
			rc = -12;	/* the vmap path i915 needs from its first GGTT setup on (N-221) */
		if (s == 3) {
			/* every IOPCIDevice into newbus (config reads only), then vgapci on the Intel display function so
			 * stage 4's drm drivers have a device to probe; the old PCI personality used to trigger this */
			IOPCIDevice *gpu = xg_find_pci(0x8086, 0x03, 0x00);
			int err = gpu ? xg_pci_xnu_start(gpu) : ENODEV;
			OSSafeReleaseNULL(gpu);
			rc = err ? -err : 1;
			xg_newbus_dump();
		}
		if (s == 4) {
			rc = xg_display_check();
			if (rc == 1 && !xg_drm_attached())
				rc = -19;	/* the init routines ran but no drm driver bound the GPU */
			if (rc == 1) {
				xg_i915_map_init();
				rc = xg_gt_check();
			}
			/* under K2 nothing scans the boot surface any more, and its GGTT range is i915's placeholder now */
			if (rc == 1 && gPipeLit && gBootSurf && xg_boot_fb_size && gKmsLevel < 3) {
				int err = xg_i915_reserve_bootfb(gBootSurf, xg_boot_fb_size);

				IOLog("XeGfx: boot surface GGTT 0x%x + %llu reserved: %d\n", gBootSurf, xg_boot_fb_size, err);
				/* -ENOSPC at or above GUC_GGTT_TOP is i915's own GuC-top reservation, which keeps it free (N-256) */
				if (err == -28 && gBootSurf < 0xfee00000u)
					rc = -28;	/* something else sits there: compositing into it would overwrite it */
			}
		}
	}
	if (s == 5)
		rc = xg_exec_check();
	if (s == 6)
		rc = xg_store_check();
	if (s == 7)
		rc = xg_accel_claim();
	if (gWdogCall)
		thread_call_cancel(gWdogCall);
	if (gTraceOn) {
		gTraceOn = false;
		thread_call_cancel(gTraceCall);
		xg_trace_write();	/* the stage's end state, s0 */
	}
	gRunning = 0;
	return rc;
}

static int
xegfx_stage_sysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
	int v = gStage, err = sysctl_handle_int(nullptr, &v, 0, req);

	if (err || !req->newptr)
		return err;
	if (v < 0 || v > 7)
		return EINVAL;
	IOLockLock(gStageLock);
	while (gStage < v) {
		int s = gStage + 1, rc = xg_run_stage(s);
		gResult[s] = rc;
		if (gResident)
			gResident->setProperty("XeGfxStage", (unsigned long long)s, 32);
		if (rc < 0)
			break;
		gStage = s;
	}
	IOLockUnlock(gStageLock);
	return 0;
}

static int
xegfx_results_sysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
	return SYSCTL_OUT(req, gResult, sizeof(gResult));
}

static void
xg_trace_write(void)
{
	static bool kept;
	char b[sizeof(gLast) + 40], line[sizeof(gLast)], accel[sizeof(gAccel)];
	IORegistryEntry *o;
	IOInterruptState is;
	bool accelNew;

	is = IOSimpleLockLockDisableInterrupt(gLogLock);
	memcpy(line, gLast, sizeof(line));
	memcpy(accel, gAccel, sizeof(accel));
	accelNew = gAccelSeq != gAccelTraceSeq;
	gAccelTraceSeq = gAccelSeq;
	gTraceSeq = gLastSeq;
	gTraceProgress = xg_progress;
	IOSimpleLockUnlockEnableInterrupt(gLogLock, is);
	for (char *p = line; *p; p++)
		if (*p == '\n' || *p == '\r')
			*p = '|';
	snprintf(b, sizeof(b), "s%d p%d %s", gRunning, xg_progress, line);
	if ((o = IORegistryEntry::fromPath("/options", gIODTPlane)) == nullptr)
		return;
	if (!kept) {
		/* the last boot's trace survives one more boot, for whoever reads it after a recovery */
		kept = true;
		if (OSObject *old = o->copyProperty("xegfx-step")) {
			o->setProperty("xegfx-step-prev", old);
			old->release();
		}
	}
	if (OSData *d = OSData::withBytes(b, (unsigned)strlen(b))) {
		o->setProperty("xegfx-step", d);
		d->release();
	}
	if (accelNew && accel[0]) {
		snprintf(b, sizeof(b), "spin %llu us/%u %s", xg_spin_max_us, xg_spin_long, accel);
		if (OSData *d = OSData::withBytes(b, (unsigned)strlen(b))) {
			o->setProperty("xegfx-accel", d);
			d->release();
		}
	}
	if (IODTNVRAM *nv = OSDynamicCast(IODTNVRAM, o))
		nv->sync();
	o->release();
}

static void
xg_trace_fire(thread_call_param_t, thread_call_param_t)
{
	uint64_t dl;

	if (!gTraceOn)
		return;
	if (gLastSeq != gTraceSeq || xg_progress != gTraceProgress || gAccelSeq != gAccelTraceSeq)
		xg_trace_write();
	clock_interval_to_deadline(250, NSEC_PER_MSEC, &dl);
	thread_call_enter_delayed(gTraceCall, dl);
}

static void
xg_hold_fire(thread_call_param_t, thread_call_param_t)
{
	panic("XeGfx: a userspace check did not finish in %d s", gHold);
}

/* write N > 0 to panic unless 0 is written within N seconds; the guard brackets its post command with it */
static int
xegfx_hold_sysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
	int v = gHold, err = sysctl_handle_int(nullptr, &v, 0, req);

	if (err || !req->newptr)
		return err;
	if (v < 0)
		return EINVAL;
	thread_call_cancel(gHoldCall);
	gHold = v;
	if (v > 0) {
		uint64_t dl;
		clock_interval_to_deadline((uint32_t)v, NSEC_PER_SEC, &dl);
		thread_call_enter_delayed(gHoldCall, dl);
	}
	return 0;
}

/* 1 = keep the NVRAM trace running outside the stages too (the guard's WindowServer step: N-251/N-255 froze there) */
static int
xegfx_trace_sysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
	int v = gTraceOn ? 1 : 0, err = sysctl_handle_int(nullptr, &v, 0, req);

	if (err || !req->newptr)
		return err;
	if (v && !gTraceOn && gTraceCall) {
		gTraceOn = true;
		xg_trace_write();
		thread_call_enter(gTraceCall);
	} else if (!v && gTraceOn) {
		gTraceOn = false;
		thread_call_cancel(gTraceCall);
	}
	return 0;
}
static int
xegfx_spin_sysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
	char b[120];

	snprintf(b, sizeof(b), "max_us %llu long %u panic %d irqs %u handled %u", xg_spin_max_us, xg_spin_long, xg_spin_panic,
	    xg_pci_irqs, xg_pci_handled);
	return SYSCTL_OUT(req, b, strlen(b) + 1);
}
SYSCTL_PROC(_debug, OID_AUTO, xegfx_spin, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, xegfx_spin_sysctl, "A",
    "the longest lock spin seen, spins over 10 ms, whether a spin past its limit panics");
SYSCTL_PROC(_debug, OID_AUTO, xegfx_trace, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, xegfx_trace_sysctl,
    "I", "1 = write the newest console line to NVRAM xegfx-step whenever it changes, outside the stages too");
SYSCTL_PROC(_debug, OID_AUTO, xegfx_stage, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, xegfx_stage_sysctl,
    "I", "highest XeGfx stage done; write N to run stages up to N (1 shim, 2 probe, 3 bus, 4 drm/i915, 5 exec, 6 store, 7 Metal accelerator)");
SYSCTL_PROC(_debug, OID_AUTO, xegfx_results, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0,
    xegfx_results_sysctl, "S", "per-stage result: 0 not run, 1 pass, negative failure");
static int
xegfx_fd_sysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
	char b[192];

	xg_fd_diag(b, sizeof(b));
	return SYSCTL_OUT(req, b, strlen(b) + 1);
}
extern "C" void xg_mem_diag(char *buf, unsigned long len);
static int
xegfx_mem_sysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
	char b[192];

	xg_mem_diag(b, sizeof(b));
	return SYSCTL_OUT(req, b, strlen(b) + 1);
}
SYSCTL_PROC(_debug, OID_AUTO, xegfx_mem, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, xegfx_mem_sysctl, "A",
    "XeGfx M_NOWAIT reserve per size class and its misses, contigmalloc failures");
SYSCTL_PROC(_debug, OID_AUTO, xegfx_fd, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, xegfx_fd_sysctl, "A",
    "XeGfx node instances by state and the fd lock holder");
SYSCTL_INT(_debug, OID_AUTO, xegfx_running, CTLFLAG_RD | CTLFLAG_LOCKED, &gRunning, 0, "stage in progress");
SYSCTL_INT(_debug, OID_AUTO, xegfx_progress, CTLFLAG_RD | CTLFLAG_LOCKED, &xg_progress, 0, "last breadcrumb");
SYSCTL_INT(_debug, OID_AUTO, xegfx_wdog, CTLFLAG_RW | CTLFLAG_LOCKED, &gWdog, 0, "stage stall panic, seconds");
SYSCTL_PROC(_debug, OID_AUTO, xegfx_hold, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, xegfx_hold_sysctl,
    "I", "panic unless reset to 0 within this many seconds (0 disarms)");
static int xegfx_log_sysctl(struct sysctl_oid *, void *, int, struct sysctl_req *);
SYSCTL_PROC(_debug, OID_AUTO, xegfx_log, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, xegfx_log_sysctl,
    "S", "the shim's console output (last 256 KiB)");
SYSCTL_STRING(_debug, OID_AUTO, xegfx_env, CTLFLAG_RW | CTLFLAG_LOCKED, xg_env, sizeof(xg_env),
    "tunables for the stages, name=value pairs");

/* M1a display hook: text the i915 display overlay left behind (read-only) */
extern "C" int xg_display_dump_text(char *out, int max);

/* read-only survey of the firmware's display handoff: write any value to re-run, read the report back */
static int
xegfx_display_sysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
	if (req->newptr)
		(void)xg_display_probe();
	return SYSCTL_OUT(req, gDisp, strlen(gDisp) + 1);
}
SYSCTL_PROC(_debug, OID_AUTO, xegfx_display, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0,
    xegfx_display_sysctl, "A", "read-only survey of the firmware's ACPI OpRegion and VBT (write to re-run)");

/* M1a: what the i915 display overlay parsed from the firmware VBT (read-only) */
static int
xegfx_disp_sysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
	/* K2a.3 filled 4 KB; the K2 commit lines come last, so the text gets a heap buffer of the dump's own size */
	const int max = 16384;
	char *buf = (char *)IOMalloc(max);
	int n, err;

	if (buf == nullptr)
		return ENOMEM;
	n = xg_display_dump_text(buf, max);
	err = n <= 0 ? ENOENT : SYSCTL_OUT(req, buf, n);
	IOFree(buf, max);
	return err;
}
SYSCTL_PROC(_debug, OID_AUTO, xegfx_disp, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0,
    xegfx_disp_sysctl, "A", "M1a: VBT dump left by the i915 display overlay (read-only)");

/* §32.13: the GPU clock -- readout, the actual MHz alone (for samplers), and the minimum (write MHz to pin it up) */
static int
xegfx_gt_sysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
	char b[256];
	int n = xg_i915_gt_text(b, sizeof(b));

	if (n < 0)
		return ENODEV;
	if ((size_t)n >= sizeof(b))
		n = (int)sizeof(b) - 1;
	return SYSCTL_OUT(req, b, n + 1);
}
SYSCTL_PROC(_debug, OID_AUTO, xegfx_gt, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0,
    xegfx_gt_sysctl, "A", "GPU clock: SLPC, actual/requested/min/max/RP0/RP1/RPn/boost MHz");
static int
xegfx_gt_act_sysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
	int v = xg_i915_gt_act_mhz();

	return SYSCTL_OUT(req, &v, sizeof(v));
}
SYSCTL_PROC(_debug, OID_AUTO, xegfx_gt_act, CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0,
    xegfx_gt_act_sysctl, "I", "actual GPU clock, MHz (0 while the GT sleeps)");
static int
xegfx_gt_min_sysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
	int v = xg_i915_gt_min_mhz(0), err;

	err = sysctl_handle_int(nullptr, &v, 0, req);
	if (err || req->newptr == USER_ADDR_NULL)
		return err;
	v = xg_i915_gt_min_mhz(v);
	return v < 0 ? EINVAL : 0;
}
SYSCTL_PROC(_debug, OID_AUTO, xegfx_gt_min, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0,
    xegfx_gt_min_sysctl, "I", "GPU minimum clock, MHz (write to set; i915 clamps it to RPn..max)");

/* §32.10: per-stage i915 execbuffer timing (i915_gem_execbuffer.c overlay); the maxes restart at every read */
extern "C" int xg_eb_stats_text(char *out, int max);
static int
xegfx_eb_sysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
	const int max = 2048;
	char *buf = (char *)IOMalloc(max);
	int n, err;

	if (buf == nullptr)
		return ENOMEM;
	n = xg_eb_stats_text(buf, max);
	err = n <= 0 ? ENOENT : SYSCTL_OUT(req, buf, n + 1);
	IOFree(buf, max);
	return err;
}
SYSCTL_PROC(_debug, OID_AUTO, xegfx_eb, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0,
    xegfx_eb_sysctl, "A", "i915 execbuffer time per stage (sum/max us; maxes restart at every read)");

/* §37.17: userptr holds (first pins of imported pages): count, pages, and where the time goes (fbsd/xg_vm.c) */
extern "C" volatile unsigned long long xg_uhold_n, xg_uhold_pages, xg_uhold_t_total, xg_uhold_t_runs, xg_uhold_t_prepare,
    xg_uhold_t_physseg, xg_uhold_t_max, xg_uhold_new_runs, xg_uhold_shared, xg_unwire_pages, xg_unwire_t;
extern "C" unsigned long long xg_surf_imports, xg_surf_pages, xg_surf_ns, xg_surf_ranges, xg_surf_frees, xg_surf_live,
    xg_surf_fail;
static int
xegfx_uhold_sysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
	char buf[900];
	uint64_t tot = 0, runs = 0, prep = 0, phys = 0, mx = 0, unw = 0;
	absolutetime_to_nanoseconds(xg_uhold_t_total, &tot); absolutetime_to_nanoseconds(xg_uhold_t_runs, &runs);
	absolutetime_to_nanoseconds(xg_uhold_t_prepare, &prep); absolutetime_to_nanoseconds(xg_uhold_t_physseg, &phys);
	absolutetime_to_nanoseconds(xg_uhold_t_max, &mx); absolutetime_to_nanoseconds(xg_unwire_t, &unw);
	const unsigned long long pg = xg_uhold_pages ? xg_uhold_pages : 1;
	int n = snprintf(buf, sizeof buf,
	    "holds %llu pages %llu (new runs %llu, shared %llu); total %llu us (%llu ns/page, max %llu us): prepare %llu us "
	    "(%llu ns/page), physseg %llu us (%llu ns/page), runs %llu us (%llu ns/page); unwire pages %llu, %llu us (%llu ns/page)",
	    xg_uhold_n, xg_uhold_pages, xg_uhold_new_runs, xg_uhold_shared, tot / 1000, tot / pg, mx / 1000, prep / 1000,
	    prep / pg, phys / 1000, phys / pg, runs / 1000, runs / pg, xg_unwire_pages, unw / 1000,
	    xg_unwire_pages ? unw / xg_unwire_pages : 0ull);
	if (n > 0 && n < (int)sizeof buf)	/* §37.17: imports by surface, in whole ns-per-page units (no %f in the kernel) */
		n += snprintf(buf + n, sizeof buf - (size_t)n,
		    "\nsurface imports %llu (%llu pages, %llu ranges, %llu ns/page), freed %llu, live %llu, failed %llu",
		    xg_surf_imports, xg_surf_pages, xg_surf_ranges, xg_surf_pages ? xg_surf_ns / xg_surf_pages : 0ull,
		    xg_surf_frees, xg_surf_live, xg_surf_fail);
	return SYSCTL_OUT(req, buf, (size_t)n + 1);
}
SYSCTL_PROC(_debug, OID_AUTO, xegfx_uhold, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0,
    xegfx_uhold_sysctl, "A", "userptr page holds: count, pages and time per phase (prepare, physseg, runs, unwire)");

/* §32.16: per-request queued->ready->done time, WindowServer vs the rest (i915_gem_execbuffer.c overlay) */
extern "C" int xg_rq_stats_text(char *out, int max);
static int
xegfx_rq_sysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
	const int max = 4096;
	char *buf = (char *)IOMalloc(max);
	int n, err;

	if (buf == nullptr)
		return ENOMEM;
	n = xg_rq_stats_text(buf, max);
	err = n <= 0 ? ENOENT : SYSCTL_OUT(req, buf, n + 1);
	IOFree(buf, max);
	return err;
}
SYSCTL_PROC(_debug, OID_AUTO, xegfx_rq, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0,
    xegfx_rq_sysctl, "A", "i915 request time queued->ready->done, WindowServer vs others (maxes and slow list restart at every read)");

/* M3d: explicit alloc/get/free exercise of the optional scanout KAPI (write to run, read the report) */
extern "C" int xg_display_kms_scanout_selftest(char *out, int max);
static char gScanoutTest[512];

static int
xegfx_kms_scanout_sysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
	if (req->newptr)
		(void)xg_display_kms_scanout_selftest(gScanoutTest, (int)sizeof(gScanoutTest));
	return SYSCTL_OUT(req, gScanoutTest, strlen(gScanoutTest) + 1);
}
SYSCTL_PROC(_debug, OID_AUTO, xegfx_kms_scanout, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0,
    xegfx_kms_scanout_sysctl, "A", "M3d: run the scanout KAPI self-test (write any value to run, read the report)");

/* M4a: what the boot-armed thread did (read-only) */
static int xg_boot_report(char *out, size_t len);

static int
xegfx_boot_sysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
	char buf[384];
	int n = xg_boot_report(buf, sizeof(buf));

	if (n <= 0)
		return EIO;
	return SYSCTL_OUT(req, buf, n + 1);
}
SYSCTL_PROC(_debug, OID_AUTO, xegfx_boot, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0,
    xegfx_boot_sysctl, "A",
    "M4a: boot-armed bring-up state, last stage/result and per-stage milliseconds (read-only)");

/* M4a timing fallback: where the nub's panel timing came from and what it says (read-only) */
static int
xegfx_timing_sysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
	char b[384];
	int n;

	if (gTimingWordsValid) {
		const uint32_t *w = gTimingWords;

		n = snprintf(b, sizeof(b),
		    "source %s tag 0x%08x pclk %u Hz %ux%u ht %u vt %u hsync %u..%u vsync %u..%u "
		    "refresh %u mHz h+ %u v+ %u measured %d edid %d vbt %d %s %s",
		    gTimingSource, w[0], w[1], w[2], w[6], w[5], w[9], w[3], w[4], w[7], w[8], w[10],
		    w[11], w[12], gTimingMeasured, gTimingEdid, gTimingVbt, gTimingChild, gTimingNote);
	} else {
		n = snprintf(b, sizeof(b), "source %s valid 0 measured %d edid %d vbt %d %s %s",
		    gTimingSource, gTimingMeasured, gTimingEdid, gTimingVbt, gTimingChild, gTimingNote);
	}
	if (n < 0)
		return EIO;
	if ((size_t)n >= sizeof(b))
		n = (int)sizeof(b) - 1;
	return SYSCTL_OUT(req, b, n + 1);
}
SYSCTL_PROC(_debug, OID_AUTO, xegfx_timing, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0,
    xegfx_timing_sysctl, "A",
    "M4a: panel timing source (measured | edid-dtd | vbt-dtd | none) and the 13 NVK5 words (read-only)");

static struct sysctl_oid *const gOids[] = {
	&sysctl__debug_xegfx_stage, &sysctl__debug_xegfx_results, &sysctl__debug_xegfx_fd, &sysctl__debug_xegfx_mem, &sysctl__debug_xegfx_running,
	&sysctl__debug_xegfx_progress, &sysctl__debug_xegfx_wdog, &sysctl__debug_xegfx_env, &sysctl__debug_xegfx_log,
	&sysctl__debug_xegfx_hold,
	&sysctl__debug_xegfx_trace,
	&sysctl__debug_xegfx_spin,
	&sysctl__debug_xegfx_display,
	&sysctl__debug_xegfx_disp,
	&sysctl__debug_xegfx_kms_scanout,
	&sysctl__debug_xegfx_boot,
	&sysctl__debug_xegfx_timing,
	&sysctl__debug_xegfx_eb,
	&sysctl__debug_xegfx_rq,
	&sysctl__debug_xegfx_uhold,
	&sysctl__debug_xegfx_gt,
	&sysctl__debug_xegfx_gt_act,
	&sysctl__debug_xegfx_gt_min,
};

/* Linux's struct va_format, the argument of "%pV" */
struct xg_va_format {
	const char	*fmt;
	va_list		*va;
};

/*
 * vsnprintf that also expands Linux's "%pV" (a nested format and its va_list), which neither XNU's nor FreeBSD's
 * printf knows; drm prints most of its messages through it (N-225 logged "MCR Steering: 0x...V"). Every other
 * conversion goes to vsnprintf one at a time with its argument taken at the right width.
 */
/* a Linux pointer extension: %p followed by a letter */
static bool
xg_has_pext(const char *f)
{
	for (; *f; f++)
		if (f[0] == '%' && f[1] == 'p' && ((f[2] >= 'A' && f[2] <= 'Z') || (f[2] >= 'a' && f[2] <= 'z')))
			return true;
	return false;
}

static int
xg_fmt(char *out, size_t len, const char *fmt, va_list *ap)
{
	size_t o = 0;

	if (!xg_has_pext(fmt))
		return vsnprintf(out, len, fmt, *ap);
	for (const char *p = fmt; *p && o + 1 < len; ) {
		if (*p != '%') {
			out[o++] = *p++;
			continue;
		}
		const char *q = p + 1;
		char spec[32];
		int lng = 0, n = 0;

		while (*q && strchr("-+ #0", *q))
			q++;
		while (*q == '*' || (*q >= '0' && *q <= '9') || *q == '.')
			q++;
		while (*q && strchr("hlqjzt", *q)) {
			if (*q == 'l' || *q == 'q' || *q == 'j' || *q == 'z' || *q == 't')
				lng++;
			q++;
		}
		if (!*q)
			break;
		if (q[0] == 'p' && q[1] == 'e') {	/* an ERR_PTR, printed as its errno */
			n = snprintf(out + o, len - o, "%ld", (long)(intptr_t)va_arg(*ap, void *));
			p = q + 2;
		} else if (q[0] == 'p' && ((q[1] >= 'A' && q[1] <= 'Z') || (q[1] >= 'a' && q[1] <= 'z')) && q[1] != 'V') {
			/* other Linux pointer extensions (%ps, %ph, %pI4...): the pointer, and skip the letters */
			n = snprintf(out + o, len - o, "%p", va_arg(*ap, void *));
			p = q + 1;
			while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9'))
				p++;
		} else if (q[0] == 'p' && q[1] == 'V') {
			struct xg_va_format *vf = va_arg(*ap, struct xg_va_format *);
			va_list inner;
			va_copy(inner, *vf->va);
			n = xg_fmt(out + o, len - o, vf->fmt, &inner);
			va_end(inner);
			p = q + 2;
		} else {
			size_t sl = MIN((size_t)(q - p + 1), sizeof(spec) - 1);
			memcpy(spec, p, sl);
			spec[sl] = 0;
			if (strchr(spec, '*'))	/* width or precision from the arguments: rare in drm, print it plainly */
				n = snprintf(out + o, len - o, "%s", spec);
			else if (*q == '%')
				n = snprintf(out + o, len - o, "%%");
			else if (*q == 's')
				n = snprintf(out + o, len - o, spec, va_arg(*ap, const char *));
			else if (*q == 'p')
				n = snprintf(out + o, len - o, spec, va_arg(*ap, void *));
			else if (*q == 'c')
				n = snprintf(out + o, len - o, spec, va_arg(*ap, int));
			else if (lng)
				n = snprintf(out + o, len - o, spec, va_arg(*ap, long long));
			else
				n = snprintf(out + o, len - o, spec, va_arg(*ap, int));
			p = q + 1;
		}
		if (n > 0)
			o = MIN(o + (size_t)n, len - 1);
	}
	out[o] = 0;
	return (int)o;
}

/* vsnprintf for the FreeBSD and Linux code (xg_bsdnames.h). Like vsnprintf it returns the full length even when
 * out is NULL or short, since drm sizes buffers with snprintf(NULL, 0, ...) */
extern "C" int
xg_vsnprintf(char *out, size_t len, const char *fmt, va_list ap)
{
	static char scratch[2048];
	va_list aq;
	int n;

	va_copy(aq, ap);
	if (!xg_has_pext(fmt)) {
		n = vsnprintf(out, len, fmt, aq);
	} else if (out == nullptr || len == 0) {
		n = xg_fmt(scratch, sizeof(scratch), fmt, &aq);	/* length only; racy scratch is fine for a count */
	} else {
		n = xg_fmt(out, len, fmt, &aq);
	}
	va_end(aq);
	return n;
}

extern "C" int
xg_snprintf(char *out, size_t len, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = xg_vsnprintf(out, len, fmt, ap);
	va_end(ap);
	return n;
}

extern "C" int
xg_vprintf(const char *fmt, va_list ap)
{
	char buf[512];
	va_list aq;
	int n;

	va_copy(aq, ap);
	n = xg_fmt(buf, sizeof(buf), fmt, &aq);
	va_end(aq);
	if (gLogLock && n > 0) {
		size_t l = MIN((size_t)n, sizeof(buf) - 1);
		IOInterruptState is = IOSimpleLockLockDisableInterrupt(gLogLock);
		if (gLogLen + l > sizeof(gLog)) {
			size_t drop = gLogLen + l - sizeof(gLog) + sizeof(gLog) / 4;
			memmove(gLog, gLog + drop, gLogLen - drop);
			gLogLen -= drop;
		}
		memcpy(gLog + gLogLen, buf, l);
		gLogLen += l;
		if (l > 1) {
			size_t have = strlen(gLast), c = MIN(l, sizeof(gLast) - 1);
			if (have + c >= sizeof(gLast)) {
				size_t drop = have + c - (sizeof(gLast) - 1);
				memmove(gLast, gLast + drop, have - drop + 1);
				have -= drop;
			}
			memcpy(gLast + have, buf, c);
			gLast[have + c] = 0;
			gLastSeq++;
		}
		IOSimpleLockUnlockEnableInterrupt(gLogLock, is);
	}
	return printf("%s", buf);
}

extern "C" int
xg_printf(const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = xg_vprintf(fmt, ap);
	va_end(ap);
	return n;
}

static int
xegfx_log_sysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
	static char snap[sizeof(gLog)];
	size_t n;

	IOLockLock(gSnapLock);	/* one reader at a time owns the snapshot; not the stage lock, a stage holds that */
	IOInterruptState is = IOSimpleLockLockDisableInterrupt(gLogLock);
	n = gLogLen;
	memcpy(snap, gLog, n);
	IOSimpleLockUnlockEnableInterrupt(gLogLock, is);
	int err = SYSCTL_OUT(req, snap, n);
	IOLockUnlock(gSnapLock);
	return err;
}

extern "C" void
xg_unimpl(const char *name)
{
	panic("XeGfx: %s is not implemented yet", name);
}

/*
 * ---- M4a: boot-armed auto bring-up ------------------------------------------------------------
 *
 * XeGfxFBClaim publishes XeGfxBootArm on IOResources immediately before XeGfxArm.  When _start sees it, the
 * stage sequence runs on a kernel thread instead of in IOKit's matching context: stage 4 runs the drm/i915
 * SYSINITs and the VBT parse, which take seconds and must not hold up matching (docs/18 §3.1).  The thread
 * waits 2 s so the claim has finished publishing, gives the sequence a 90 s budget, clears the stage-stall
 * watchdog (an unattended boot must not panic) and stops at the first failing stage.  Stage 4's
 * intel_bios_init publishes XeGfxDisplayNub as a side effect of compat.linuxkpi.i915_xegfx_disp_level=2, which
 * is why the tunable is appended to xg_env here and not on the manual path.
 */
#define XG_BOOT_DELAY_MS	2000
#define XG_BOOT_BUDGET_NS	(90ull * NSEC_PER_SEC)
#define XG_BOOT_IDLE		0
#define XG_BOOT_WAITING		1
#define XG_BOOT_RUNNING		2
#define XG_BOOT_READY		3
#define XG_BOOT_FAILED		4
#define XG_BOOT_TIMEOUT		5
#define XG_BOOT_ABORTED		6

static volatile int gBootArmed;		/* _start started the boot thread; _stop must not unload */
static volatile int gBootPhase = XG_BOOT_IDLE;
static volatile int gBootDone;		/* highest stage the boot thread finished */
static volatile int gBootResult;	/* 1 after stage 7, else the stage result that stopped it */
static uint64_t gBootStageMs[8];	/* per-stage wall time, milliseconds */
static int gBootTrigger;		/* 1 XeGfxBootArm resource, 2 NVRAM xegfx-disp-bootarm, 3 both */
static int gBootAgdcResult;		/* 1 = debug.xegfx_agdc read back 1, negative = failed (warn only) */
static int gBootK5Result;
static uint64_t gBootAgdcMs, gBootK5Ms;
#define XG_BOOT_AGDC_POLL_MS	250
#define XG_BOOT_AGDC_MAX_MS	10000

static const char *
xg_boot_phase_name(int phase)
{
	switch (phase) {
	case XG_BOOT_WAITING:	return "waiting";
	case XG_BOOT_RUNNING:	return "running";
	case XG_BOOT_READY:	return "ready";
	case XG_BOOT_FAILED:	return "failed";
	case XG_BOOT_TIMEOUT:	return "timeout";
	case XG_BOOT_ABORTED:	return "aborted";
	default:		return "idle";
	}
}

static const char *
xg_boot_trigger_name(int trigger)
{
	switch (trigger) {
	case 1:		return "resource";
	case 2:		return "nvram";
	case 3:		return "resource+nvram";
	default:	return "manual";
	}
}

/* XeGfxBootArm / XeGfxBootAbort: a resource property set by XeGfxFBClaim (or a test) as OSBoolean or OSNumber */
static bool
xg_resource_flag(const char *key)
{
	IOService *res = IOService::getResourceService();
	OSObject *o;

	if (res == nullptr || (o = res->getProperty(key)) == nullptr)
		return false;
	if (OSBoolean *b = OSDynamicCast(OSBoolean, o))
		return b->isTrue();
	if (OSNumber *n = OSDynamicCast(OSNumber, o))
		return n->unsigned32BitValue() != 0;
	return false;
}

/* M4a-1: /options xegfx-disp-bootarm; OpenCore rewrites boot-args each boot, NVRAM /options persists */
static bool
xg_nvram_flag(const char *key)
{
	IORegistryEntry *o = IORegistryEntry::fromPath("/options", gIODTPlane);
	OSObject *v;
	bool armed = false;

	if (o == nullptr)
		return false;
	if ((v = o->copyProperty(key)) != nullptr) {
		if (OSBoolean *b = OSDynamicCast(OSBoolean, v)) {
			armed = b->isTrue();
		} else if (OSNumber *n = OSDynamicCast(OSNumber, v)) {
			armed = n->unsigned32BitValue() != 0;
		} else if (OSData *d = OSDynamicCast(OSData, v)) {
			const uint8_t *p = (const uint8_t *)d->getBytesNoCopy();
			unsigned int len = d->getLength();

			if (p != nullptr && len >= 4)
				armed = xg_le32(p) != 0;
			else if (p != nullptr && len >= 1)
				armed = p[0] != 0;
		}
		v->release();
	}
	o->release();
	return armed;
}

/* Consume an explicitly armed trial before entering any GPU stage. */
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

/* K4: NVRAM xegfx-disp-bootmode, 16 bytes LE: 'XBM1', refresh mHz, desktop width, height (the adapter writes it) */
static bool
xg_nvram_boot_mode(uint32_t *mhz, uint32_t *w, uint32_t *h)
{
	IORegistryEntry *o = IORegistryEntry::fromPath("/options", gIODTPlane);
	bool ok = false;

	if (o == nullptr)
		return false;
	if (OSObject *v = o->copyProperty("xegfx-disp-bootmode")) {
		OSData *d = OSDynamicCast(OSData, v);
		const uint8_t *p = d != nullptr ? (const uint8_t *)d->getBytesNoCopy() : nullptr;

		if (p != nullptr && d->getLength() == 16 && xg_le32(p) == 0x314d4258u) {
			*mhz = xg_le32(p + 4);
			*w = xg_le32(p + 8);
			*h = xg_le32(p + 12);
			ok = *mhz != 0;
		}
		v->release();
	}
	o->release();
	return ok;
}

/* same /options lookup, as a number (4-byte LE data, OSNumber, or a single byte); absent reads 0 */
static uint32_t
xg_nvram_u32(const char *key)
{
	IORegistryEntry *o = IORegistryEntry::fromPath("/options", gIODTPlane);
	OSObject *v;
	uint32_t val = 0;

	if (o == nullptr)
		return 0;
	if ((v = o->copyProperty(key)) != nullptr) {
		if (OSNumber *n = OSDynamicCast(OSNumber, v)) {
			val = n->unsigned32BitValue();
		} else if (OSData *d = OSDynamicCast(OSData, v)) {
			const uint8_t *p = (const uint8_t *)d->getBytesNoCopy();
			unsigned int len = d->getLength();

			if (p != nullptr && len >= 4)
				val = xg_le32(p);
			else if (p != nullptr && len >= 1)
				val = p[0];
		}
		v->release();
	}
	o->release();
	return val;
}

static void
xg_boot_publish(const char *state)
{
	IOService *res;

	if (gResident)
		gResident->setProperty("XeGfxBootState", state);
	if ((res = IOService::getResourceService()) != nullptr)
		res->setProperty("XeGfxBootState", state);
}

/* debug.xegfx_boot: one line, so it can be read from a boot script without ioreg */
static int
xg_boot_report(char *out, size_t len)
{
	int n = snprintf(out, len,
	    "armed %d phase %s stage %d result %d trigger %s ms %llu %llu %llu %llu %llu %llu %llu "
	    "agdc %d %llu k5 %d %llu",
	    gBootArmed, xg_boot_phase_name(gBootPhase), gBootDone, gBootResult,
	    xg_boot_trigger_name(gBootTrigger),
	    (unsigned long long)gBootStageMs[1], (unsigned long long)gBootStageMs[2],
	    (unsigned long long)gBootStageMs[3], (unsigned long long)gBootStageMs[4],
	    (unsigned long long)gBootStageMs[5], (unsigned long long)gBootStageMs[6],
	    (unsigned long long)gBootStageMs[7],
	    gBootAgdcResult, (unsigned long long)gBootAgdcMs, gBootK5Result, (unsigned long long)gBootK5Ms);

	if (n < 0)
		return -EIO;
	if ((size_t)n >= len)
		n = (int)len - 1;
	return n;
}

/*
 * M4a-1: the manual bring-up script enabled the AGDC/K5 chain after stages 4..7 (docs/15:243,
 * `sysctl -w debug.xegfx_agdc=1`, then debug.xegfx_agdc_k5=1).  Those two sysctls belong to the AGDC kext; a
 * kernel caller can drive them through sysctlbyname (the same call xg_fd.cpp:136 makes, so the symbol links),
 * so the boot-armed path does the two writes and polls the read-back value.  A failure only warns: the timing
 * and adapter chain must still be published.
 */
extern "C" int sysctlbyname(const char *, void *, size_t *, void *, size_t);

static int
xg_boot_sysctl_read(const char *name)
{
	int v = -1;
	size_t len = sizeof(v);

	if (sysctlbyname(name, &v, &len, NULL, 0) != 0)
		return -1;
	return v;
}

static int
xg_boot_sysctl_write(const char *name, int v)
{
	return sysctlbyname(name, NULL, NULL, &v, sizeof(v));
}

static void
xg_boot_agdc_wait(const char *name, const char *label, int *result, uint64_t *ms_out)
{
	uint64_t t0 = mach_absolute_time(), ns;
	int v = -1, rc;

	rc = xg_boot_sysctl_write(name, 1);
	if (rc != 0) {
		*result = rc;
		*ms_out = 0;
		IOLog("XeGfx: boot %s: write %s=1 failed (%d); AGDC kext not loaded or sysctl missing\n",
		    label, name, rc);
		return;
	}
	for (;;) {
		v = xg_boot_sysctl_read(name);
		if (v == 1) {
			*result = 1;
			break;
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
		if (ns / NSEC_PER_MSEC >= XG_BOOT_AGDC_MAX_MS) {
			*result = -ETIMEDOUT;
			break;
		}
		IOSleep(XG_BOOT_AGDC_POLL_MS);
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	*ms_out = ns / NSEC_PER_MSEC;
	if (*result == 1)
		IOLog("XeGfx: boot %s: %s=1 read back in %llu ms\n", label, name, (unsigned long long)*ms_out);
	else
		IOLog("XeGfx: boot %s: %s did not read back 1 within %u ms (last %d); continuing\n",
		    label, name, XG_BOOT_AGDC_MAX_MS, v);
}

static void
xg_boot_agdc_steps(void)
{
	xg_boot_agdc_wait("debug.xegfx_agdc", "AGDC", &gBootAgdcResult, &gBootAgdcMs);
	xg_boot_agdc_wait("debug.xegfx_agdc_k5", "K5", &gBootK5Result, &gBootK5Ms);
}

static void
xg_boot_publish_agdc(void)
{
	IOService *res = IOService::getResourceService();

	if (gResident) {
		gResident->setProperty("XeGfxBootAgdc", (unsigned long long)gBootAgdcResult, 32);
		gResident->setProperty("XeGfxBootAgdcMs", gBootAgdcMs, 64);
		gResident->setProperty("XeGfxBootK5", (unsigned long long)gBootK5Result, 32);
		gResident->setProperty("XeGfxBootK5Ms", gBootK5Ms, 64);
	}
	if (res != NULL) {
		res->setProperty("XeGfxBootAgdc", (unsigned long long)gBootAgdcResult, 32);
		res->setProperty("XeGfxBootAgdcMs", gBootAgdcMs, 64);
		res->setProperty("XeGfxBootK5", (unsigned long long)gBootK5Result, 32);
		res->setProperty("XeGfxBootK5Ms", gBootK5Ms, 64);
	}
}

static void
xg_boot_run(void *, wait_result_t)
{
	uint64_t t0, b, elapsed, dur;
	int prev_wdog, rc = 1;

	gBootPhase = XG_BOOT_RUNNING;
	xg_boot_publish("running");
	IOLog("XeGfx: boot-armed: stages 1..7 start in %u ms, budget %llu s, stage watchdog off\n",
	    XG_BOOT_DELAY_MS, XG_BOOT_BUDGET_NS / NSEC_PER_SEC);
	IOSleep(XG_BOOT_DELAY_MS);

	t0 = mach_absolute_time();
	prev_wdog = gWdog;
	gWdog = 0;	/* an unattended boot must not panic: the thread stops at the failing stage */
	IOLockLock(gStageLock);
	for (int s = 1; s <= 7; s++) {
		if (gStage >= s) {
			gBootDone = s;	/* a manual sysctl won the race before the delay expired */
			continue;
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - t0, &elapsed);
		if (elapsed > XG_BOOT_BUDGET_NS) {
			gBootPhase = XG_BOOT_TIMEOUT;
			gBootResult = -ETIMEDOUT;
			goto out;
		}
		if (xg_resource_flag("XeGfxBootAbort")) {
			gBootPhase = XG_BOOT_ABORTED;
			gBootResult = -ECANCELED;
			goto out;
		}
		IOLog("XeGfx: boot stage %d start (t+%llu ms)\n", s, elapsed / NSEC_PER_MSEC);
		b = mach_absolute_time();
		rc = xg_run_stage(s);
		absolutetime_to_nanoseconds(mach_absolute_time() - b, &dur);
		absolutetime_to_nanoseconds(mach_absolute_time() - t0, &elapsed);
		gResult[s] = rc;
		gBootStageMs[s] = dur / NSEC_PER_MSEC;
		if (gResident) {
			gResident->setProperty("XeGfxStage", (unsigned long long)s, 32);
			gResident->setProperty("XeGfxStageMs", (unsigned long long)gBootStageMs[s], 32);
		}
		IOLog("XeGfx: boot stage %d rc=%d in %llu ms (t+%llu ms)\n", s, rc,
		    gBootStageMs[s], elapsed / NSEC_PER_MSEC);
		if (rc < 0) {
			gBootPhase = XG_BOOT_FAILED;
			gBootResult = rc;
			goto out;
		}
		gStage = s;
		gBootDone = s;
	}
	gBootPhase = XG_BOOT_READY;
	gBootResult = 1;
out:
	IOLockUnlock(gStageLock);
	gWdog = prev_wdog;
	if (gBootPhase == XG_BOOT_READY) {
		/*
		 * M4a-1: AGDC and K5 are enabled before the core is announced, because the accelerator's boot
		 * thread waits for the head gate after stage 7 and expects them ready.  Not fatal: record and warn.
		 */
		xg_boot_agdc_steps();
		xg_boot_publish_agdc();
		/* a resource, not just a property: adapter/accel can gate on IOResourceMatch XeGfxCoreReady */
		IOService::publishResource("XeGfxCoreReady", kOSBooleanTrue);
		IOLog("XeGfx: M4a core ready: stages 1..7 passed, AGDC/K5 %d/%d, XeGfxCoreReady published\n",
		    gBootAgdcResult, gBootK5Result);
	} else {
		IOLog("XeGfx: M4a bring-up stopped (%s, stage %d, rc %d); manual debug.xegfx_stage unchanged\n",
		    xg_boot_phase_name(gBootPhase), gBootDone, gBootResult);
	}
	xg_boot_publish(xg_boot_phase_name(gBootPhase));
	gBootArmed = 0;
	thread_terminate(current_thread());
}

extern "C" kern_return_t
_start(kmod_info_t *ki, void *)
{
	gKextBase = ki ? ki->address : 0;
	gIrqoffCall = thread_call_allocate(xg_irqoff_check, nullptr);
	gStageLock = IOLockAlloc();
	gLogLock = IOSimpleLockAlloc();
	gSnapLock = IOLockAlloc();
	gWdogCall = thread_call_allocate(xg_wdog_fire, nullptr);
	gHoldCall = thread_call_allocate(xg_hold_fire, nullptr);
	gTraceCall = thread_call_allocate(xg_trace_fire, nullptr);
	if (!gStageLock || !gWdogCall || !gHoldCall || !gTraceCall || !gLogLock || !gSnapLock)
		return KERN_FAILURE;
	/*
	 * i915 stays off the display until XeGfx owns it, and gets the GuC explicitly: drm-kmod leaves enable_guc at -1 on
	 * FreeBSD ("GuC is known to be broken"), which on Meteor/Arrow Lake means no submission at all and a wedged GT
	 * (Legion N-225 to N-227). The guard can override both per experiment.
	 */
	strlcpy(xg_env, "compat.linuxkpi.i915_disable_display=1 compat.linuxkpi.i915_enable_guc=3", sizeof(xg_env));
	for (auto *o : gOids)
		sysctl_register_oid(o);
	IOService::publishResource("XeGfxArm");
	/*
	 * M4a: XeGfxFBClaim publishes XeGfxBootArm just before XeGfxArm.  The stages cannot run here: this is
	 * IOKit's resource-matching context and stage 4 runs the drm/i915 SYSINITs.  The thread waits 2 s so the
	 * claim has published everything, then runs them while matching returns now and the adapter's
	 * XeGfxFramebuffer can probe the nub stage 4 publishes.
	 */
	{
		/* M4a-1: two independent triggers; the M4a-1 daemon path has no resource, only NVRAM /options */
		int res_armed = xg_resource_flag("XeGfxBootArm") ? 1 : 0;
		int nv_armed = xg_nvram_flag("xegfx-disp-bootarm") ? 1 : 0;

		gBootTrigger = xg_trial_once_begin() ? (res_armed | (nv_armed << 1)) : 0;
		xg_display_set_external_dp(xg_nvram_flag("xegfx-disp-external-dp") ? 1 : 0);
		if (gBootTrigger != 0) {
			thread_t th = nullptr;
			IOService *res = IOService::getResourceService();

			/* level 2 makes intel_bios_init publish XeGfxDisplayNub after parsing the VBT (xg_display_hook.c) */
			strlcat(xg_env, " compat.linuxkpi.i915_xegfx_disp_level=2", sizeof(xg_env));
			/*
			 * K1 staged eDP takeover (NVRAM xegfx-disp-kms=1): the real display runs -- i915 builds the
			 * internal panel's outputs and reads the firmware pipe back -- but commits nothing (the overlays
			 * gate on compat.linuxkpi.i915_xegfx_kms).  Claim boots only; anything else keeps the display off.
			 */
			{
				/* 1 = K1 read-only adoption, 2 = K2a: K1 + atomic check only, 3 = K2: + one 60 Hz commit; higher -> 3 */
				uint32_t kms = xg_nvram_u32("xegfx-disp-kms");
				if (kms > 3)
					kms = 3;
				if (kms >= 1) {
					char *dd = (char *)strstr_kpi(xg_env, "i915_disable_display=1");
					char kv[192];
					if (dd != nullptr)
						dd[sizeof("i915_disable_display=") - 1] = '0';
					/*
					 * K2a+: no PSR -- the accel writes PLANE_SURF itself and DMC is missing (docs/03-PLAN §30.6).
					 * No DPT either (§32.16): a Tile4 plane then reads the GGTT directly, so the accel's slots
					 * work for Tile4 frames as they do for linear ones (with DPT, PLANE_SURF names a page table).
					 */
					snprintf(kv, sizeof(kv), " compat.linuxkpi.i915_xegfx_no_display=0 compat.linuxkpi.i915_xegfx_kms=%u%s",
					    kms, kms >= 2 ? " compat.linuxkpi.i915_enable_psr=0 compat.linuxkpi.i915_enable_dpt=0" : "");
					strlcat(xg_env, kv, sizeof(xg_env));
					IOLog("XeGfx: staged eDP takeover armed by NVRAM xegfx-disp-kms=%u (%s)\n", kms,
					    kms == 1 ? "K1 read-only adoption" : kms == 2 ? "K2a read-only adoption + atomic check" :
					    "K2 adoption + atomic check + 60 Hz commit");
				}
				gKmsLevel = kms;
				if (gResident)
					gResident->setProperty("XeGfxKmsLevel", (unsigned long long)kms, 32);
				if (kms >= 3) {
					uint32_t mhz = 0, bw = 0, bh = 0;

					if (xg_nvram_boot_mode(&mhz, &bw, &bh)) {
						xg_display_set_boot_mode(mhz, bw, bh);
						IOLog("XeGfx: K4 boot mode from NVRAM xegfx-disp-bootmode: %u.%03u Hz, desktop %ux%u\n",
						    mhz / 1000, mhz % 1000, bw, bh);
					}
				}
			}
			gBootArmed = 1;
			gBootPhase = XG_BOOT_WAITING;
			if (gResident)
				gResident->setProperty("XeGfxBootTrigger",
				    xg_boot_trigger_name(gBootTrigger));
			if (res != nullptr)
				res->setProperty("XeGfxBootTrigger",
				    xg_boot_trigger_name(gBootTrigger));
			if (kernel_thread_start(xg_boot_run, nullptr, &th) == KERN_SUCCESS) {
				thread_deallocate(th);	/* the thread holds its own reference until thread_terminate() */
				IOLog("XeGfx: boot-armed by %s: stages 1..7 scheduled on a kernel thread\n",
				    gBootTrigger == 1 ? "XeGfxBootArm resource" :
				    gBootTrigger == 2 ? "NVRAM xegfx-disp-bootarm" :
				    "XeGfxBootArm resource + NVRAM xegfx-disp-bootarm");
			} else {
				gBootArmed = 0;
				gBootPhase = XG_BOOT_FAILED;
				gBootResult = -ENOMEM;
				xg_boot_publish("thread-start-failed");
				IOLog("XeGfx: boot-armed by %s but kernel_thread_start failed; stages stay manual\n",
				    xg_boot_trigger_name(gBootTrigger));
			}
		}
	}
	return KERN_SUCCESS;
}

extern "C" kern_return_t
_stop(kmod_info_t *, void *)
{
	if (gStage > 0 || gBootArmed)
		return KERN_FAILURE;	/* no teardown yet / the boot thread still owns the stages */
	for (auto *o : gOids)
		sysctl_unregister_oid(o);
	thread_call_free(gWdogCall);
	thread_call_cancel(gHoldCall);
	thread_call_free(gHoldCall);
	thread_call_cancel(gTraceCall);
	thread_call_free(gTraceCall);
	IOLockFree(gStageLock);
	IOSimpleLockFree(gLogLock);
	IOLockFree(gSnapLock);
	return KERN_SUCCESS;
}
