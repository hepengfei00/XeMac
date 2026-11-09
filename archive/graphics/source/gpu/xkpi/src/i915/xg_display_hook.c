// SPDX-License-Identifier: MIT
/*
 * M1a: read the VBT that intel_bios_init() parsed, with the display engine left
 * alone.
 *
 * intel_bios_init() only parses when HAS_DISPLAY() is true, so the overlayed
 * intel_display_driver.c fakes that for exactly that call (see the comment
 * there) and puts the display back to sleep immediately afterwards.  This file
 * only formats what the parser produced; it touches no display register.
 */

#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <drm/drm_print.h>

#include "i915_drv.h"
#include "intel_display_device.h"
#include "intel_display_types.h"

#include "xg_display_hook.h"
#include "xegfx_external_dp_policy.h"
#include "xg_i915.h"
#include "xg_vm_xnu.h"

extern int getenv_int(const char *name, int *data);
/* thread-context guard for the scanout allocator (xg_sched_xnu.h / Kernel.framework) */
extern int preemption_enabled(void);
extern int ml_get_interrupts_enabled(void);

#define XG_DISPLAY_DUMP_MAX	16384	/* K2a.3 overflowed 8 KB with the K1 readout + checks */

static int gXgExternalDp;
static int gXgDpChildIndex = -1;

void xg_display_set_external_dp(int enabled)
{
	gXgExternalDp = enabled != 0;
}

int xg_display_external_dp(void)
{
	return gXgExternalDp;
}


static int	gXgDispLevel = -1;
static char	gXgDispDump[XG_DISPLAY_DUMP_MAX];
static int	gXgDispDumpLen;

int
xg_display_disp_level(void)
{
	int level = 0;

	if (gXgDispLevel >= 0)
		return (gXgDispLevel);
	getenv_int("compat.linuxkpi.i915_xegfx_disp_level", &level);
	if (level < 0 || level > 3)
		level = 0;
	gXgDispLevel = level;
	return (level);
}

static void
xg_disp_line(struct intel_display *display, const char *line)
{
	int n;

	drm_info(display->drm, "XEGFX_DISP: %s\n", line);
	n = strlen(line);
	if (gXgDispDumpLen + n + 2 > (int)sizeof(gXgDispDump))
		return;
	memcpy(gXgDispDump + gXgDispDumpLen, line, n);
	gXgDispDumpLen += n;
	gXgDispDump[gXgDispDumpLen++] = '\n';
	gXgDispDump[gXgDispDumpLen] = '\0';
}

void
xg_display_note(struct intel_display *display, const char *line)
{
	xg_disp_line(display, line);
}

void
xg_display_dump_vbt(struct intel_display *display)
{
	struct list_head *pos;
	char line[192];
	int blocks = 0;
	int devices = 0;

	gXgDispDumpLen = 0;
	gXgDispDump[0] = '\0';
	xg_display_aux_set_display(display);	/* the nub's AUX read needs it after intel_bios_init() */

	snprintf(line, sizeof(line), "level=%d", xg_display_disp_level());
	xg_disp_line(display, line);
	snprintf(line, sizeof(line), "HAS_DISPLAY=%d pipe_mask=0x%x",
	    HAS_DISPLAY(display) ? 1 : 0,
	    (unsigned)DISPLAY_RUNTIME_INFO(display)->pipe_mask);
	xg_disp_line(display, line);
	snprintf(line, sizeof(line), "opregion=%s",
	    display->opregion ? "present" : "absent");
	xg_disp_line(display, line);
	snprintf(line, sizeof(line), "vbt.header_version=%u",
	    (unsigned)display->vbt.version);
	xg_disp_line(display, line);
	list_for_each(pos, &display->vbt.bdb_blocks)
		blocks++;
	list_for_each(pos, &display->vbt.display_devices)
		devices++;
	snprintf(line, sizeof(line), "bdb_blocks=%d child_devices=%d",
	    blocks, devices);
	xg_disp_line(display, line);
}


/*
 * M2c: the raw VBT image and the raw child_device_config records, copied here at
 * parse time so the display nub can hand them to the adapter kext as OSData.
 * The copies live in the kernel heap and are published once; number/size are
 * small (VBT <= 8 KiB, <= 8 children of 64 B).
 */
static unsigned char	gXgVbtData[XG_DISPLAY_MAX_VBT];
static unsigned int	gXgVbtLen;
static unsigned char	gXgChildData[XG_DISPLAY_MAX_CHILDREN * XG_DISPLAY_MAX_CHILD_SIZE];
static unsigned int	gXgChildSize;
static unsigned int	gXgChildStride;
static int		gXgChildCount;

void
xg_display_set_vbt(const void *data, unsigned int len)
{
	if (data == NULL || len == 0)
		return;
	if (len > sizeof(gXgVbtData))
		len = sizeof(gXgVbtData);
	memcpy(gXgVbtData, data, len);
	gXgVbtLen = len;
}

unsigned int
xg_display_vbt_get(const void **data)
{
	if (data != NULL)
		*data = (const void *)gXgVbtData;
	return (gXgVbtLen);
}

void
xg_display_add_child(const void *data, unsigned int len)
{
	if (data == NULL || len == 0 ||
	    gXgChildCount >= XG_DISPLAY_MAX_CHILDREN)
		return;
	if (len > (unsigned int)XG_DISPLAY_MAX_CHILD_SIZE)
		len = XG_DISPLAY_MAX_CHILD_SIZE;
	/*
	 * M3b: pack the records back to back with the size actually recorded by
	 * intel_bios.c (40 B on this VBT), so that child_count * child_size is
	 * exactly the published vbt-children image.  The previous code stored
	 * every record at a 64 B stride but reported a 40 B stride, leaving
	 * 24 B of padding between records in the nub property.
	 */
	if (gXgChildCount == 0)
		gXgChildStride = len;
	else if (len != gXgChildStride)
		return;
	if ((unsigned int)(gXgChildCount + 1) * gXgChildStride >
	    (unsigned int)sizeof(gXgChildData))
		return;
	memcpy(gXgChildData + (unsigned int)gXgChildCount * gXgChildStride,
	    data, len);
	gXgChildSize = gXgChildStride;
	gXgChildCount++;
}

int
xg_display_children_get(const void **data, unsigned int *child_size)
{
	if (data != NULL)
		*data = (const void *)gXgChildData;
	if (child_size != NULL)
		*child_size = gXgChildSize;
	return (gXgChildCount);
}

int
xg_display_dump_text(char *out, int max)
{
	int n;

	if (gXgDispDumpLen <= 0 || max <= 0)
		return (0);
	n = gXgDispDumpLen < max - 1 ? gXgDispDumpLen : max - 1;
	memcpy(out, gXgDispDump, n);
	out[n] = '\0';
	return (n);
}

/*
 * ============================================================================
 * M3b: read-only KMS KAPI v1
 * ============================================================================
 *
 * The display nub publishes the address of gXgKmsKapi as an 8-byte OSData
 * ("xegfx-kms-kapi").  The table is static const data owned by this kext; it
 * stays valid until the kext is unloaded, so the nub and every adapter that
 * holds the pointer may use it for their whole lifetime.  No entry point
 * sleeps, allocates, takes a lock, or touches a display register.
 *
 * ABI rules (also in docs/11-M3B-KAPI.md):
 *  - all members are naturally aligned; the header is four u32 words and the
 *    function-pointer block starts at offset 16 (8-byte aligned on x86_64);
 *  - every pointer passed in must be a valid kernel pointer; the KAPI never
 *    copies to/from user space and never returns a kernel pointer;
 *  - "out" structs start with a u32 "size" that the caller sets to
 *    sizeof(struct); the callee rejects a mismatch instead of guessing;
 *  - entry points are callable from kernel thread context only.  They must
 *    not be called from interrupt context (v1 has no such caller);
 *  - the table is versioned as a whole: any change to a struct layout, an
 *    error code, or a non-NULL slot requires a version bump.  A NULL slot
 *    means "not implemented in this version" -> return XG_KMS_ERR_UNSUPPORTED
 *    without calling it.
 */

#define XG_KMS_KAPI_SIZE		136U
#define XG_KMS_KAPI_FLAG_READ_ONLY	(1U << 0)
/*
 * K3 additive modeset extension (same table, version 1): getTiming(fbIndex = XG_KMS_FIXED_MODE_BASE + n) returns the
 * panel's fixed mode n as the same 13 words, and setMode(ctx, 0, n) commits it through i915 (docs/03-PLAN §30.17).
 * Both answer NOT_READY until a K2 (kms>=3) boot has committed once; a reader that only knows v1 never sets the bit.
 */
#define XG_KMS_KAPI_FLAG_MODESET	(1U << 2)
#define XG_KMS_FIXED_MODE_BASE		0x100U
#define XG_KMS_MAX_FIXED_MODES		4
/*
 * K4 boot mode (native-display docs/32-FRAME-RATE.md §32.20): the boot commit runs the mode the adapter last set
 * (NVRAM xegfx-disp-bootmode: refresh and desktop) instead of the panel's 60 Hz.  getTiming(fbIndex =
 * XG_KMS_BOOT_STATE_INDEX) returns what the boot committed: word 0 XG_KMS_BOOT_STATE_TAG, 1 the fixed mode index, 2/3
 * the desktop on the plane, 4 its pitch, 5 joined, 6 1 when it is the saved mode.  NOT_FOUND on an older core.
 */
#define XG_KMS_BOOT_STATE_INDEX		0x200U
#define XG_KMS_BOOT_STATE_TAG		0x4b340001U
/*
 * A1 additive scaled-desktop extension (same table, version 1): reserved[3] is setPlane(ctx, width, height, pitch),
 * which puts a width x height desktop (linear XRGB8888, pitch bytes per row) on the primary plane scaled onto the
 * current raster -- i915 assigns the plane scaler and recomputes the watermarks.  NOT_READY before a K2 commit.
 */
#define XG_KMS_KAPI_FLAG_PLANE		(1U << 3)
#define XG_KMS_PLANE_MAX_W		3840U	/* the 1920x1200 HiDPI backing */
#define XG_KMS_PLANE_MAX_H		2400U
static unsigned int xg_kms_fixed_mode_timing(unsigned int n, unsigned int *out13);
static unsigned int xg_kms_boot_state(unsigned int *out13);
static unsigned int xg_kms_current_timing(unsigned int *out13);
static unsigned int xg_kms_set_mode(void *ctx, unsigned int fb_index, unsigned int mode_id);
static unsigned int xg_kms_set_plane(void *ctx, unsigned int width, unsigned int height, unsigned int pitch);
static unsigned int xg_kms_cursor_image(void *ctx, const unsigned int *argb, unsigned int width, unsigned int height,
    unsigned int pitch);
static unsigned int xg_kms_cursor_move(void *ctx, int x, int y, unsigned int visible);
static void xg_kms_cursor_reapply(void);
/*
 * B1 additive backlight extension (same table, version 1): reserved[4] is backlight(ctx, set, &percent) -- set >= 0
 * sets the panel brightness to that percent through i915's own backlight (intel_backlight_set_acpi(), the path the
 * ACPI/OpRegion brightness requests take), set < 0 only reads; percent returns the level now.
 */
#define XG_KMS_KAPI_FLAG_BACKLIGHT	(1U << 4)
static unsigned int xg_kms_backlight(void *ctx, int set_percent, unsigned int *percent_out);
/*
 * E3 additive vblank extension (same table, version 1; native-display docs/32-FRAME-RATE.md): reserved[5] is
 * waitVblank(ctx, last, timeoutUs, &count, &ageNs).  It blocks until pipe A's vblank count is no longer `last` (or the
 * timeout), then returns the count and how long ago, by i915's own scanline-corrected vblank timestamp, that vblank
 * happened.  An age rather than a time: ktime in this kext counts from its own load, the adapter's clocks do not.
 * BUSY when the pipe is off (a modeset in flight) or no vblank came in time.
 */
#define XG_KMS_KAPI_FLAG_VBLANK		(1U << 5)
static unsigned int xg_kms_wait_vblank(void *ctx, unsigned long long last, unsigned int timeout_us,
    unsigned long long *count_out, unsigned long long *age_ns_out);
struct intel_panel;
static unsigned int xg_kms_backlight_pct(const struct intel_panel *panel);

#define XG_KMS_TIMING_TAG		0x4b350001U
#define XG_KMS_TIMING_WORDS		13
#define XG_KMS_TIMING_BYTES		(XG_KMS_TIMING_WORDS * 4U)

#define XG_KMS_CONNECTOR_WORDS		46
#define XG_KMS_CONNECTOR_BYTES		(XG_KMS_CONNECTOR_WORDS * 4U)
#define XG_KMS_CHILD_BYTES		40

#define XG_KMS_OK			0U
#define XG_KMS_ERR_BAD_ARGUMENT		0xE00002C2U	/* kIOReturnBadArgument */
#define XG_KMS_ERR_UNSUPPORTED		0xE00002C7U	/* kIOReturnUnsupported */
#define XG_KMS_ERR_BUSY			0xE00002D5U	/* kIOReturnBusy */
#define XG_KMS_ERR_NOT_READY		0xE00002D8U	/* kIOReturnNotReady */
#define XG_KMS_ERR_NOT_FOUND		0xE00002F0U	/* kIOReturnNotFound */
#define XG_KMS_ERR_NO_MEMORY		0xE00002BDU	/* kIOReturnNoMemory */

enum {
	XG_KMS_CONN_EDP = 0,
	XG_KMS_CONN_DP = 1,
	XG_KMS_CONN_HDMI = 2,
	XG_KMS_CONN_DSI = 3,
	XG_KMS_CONN_UNKNOWN = 4,
};

#define XG_KMS_CONN_FLAG_INTERNAL	(1U << 0)
#define XG_KMS_CONN_FLAG_DP		(1U << 1)
#define XG_KMS_CONN_FLAG_HDMI		(1U << 2)
#define XG_KMS_CONN_FLAG_AUX		(1U << 3)
#define XG_KMS_CONN_FLAG_LSPCON		(1U << 4)
#define XG_KMS_CONN_FLAG_EFP_ROUTED	(1U << 5)

/*
 * 13 u32 words, exactly the NVK5_TIMING_WORDS image produced by the NVRM AGDC
 * producer (kexts/NVRMAGDC/xe/panel-timing.h) and published by NVRM as
 * "XeGfxMeasuredPanelTiming".  All words are little-endian u32, packed.
 */
struct xg_kms_timing {
	unsigned int	tag;			/* XG_KMS_TIMING_TAG */
	unsigned int	pclk_khz;		/* pixel clock, kHz */
	unsigned int	hvisible;
	unsigned int	hsync_start;
	unsigned int	hsync_end;
	unsigned int	htotal;
	unsigned int	vvisible;
	unsigned int	vsync_start;
	unsigned int	vsync_end;
	unsigned int	vtotal;
	unsigned int	refresh_hz;		/* word 10: Hz (producer name says MHz) */
	unsigned int	hsync_positive;		/* 1 = positive polarity */
	unsigned int	vsync_positive;
};

_Static_assert(sizeof(struct xg_kms_timing) == XG_KMS_TIMING_BYTES,
    "xg_kms_timing must stay 13 u32 words");

/*
 * 46 u32 words.  This is the static part of one VBT child_device_config as
 * parsed by i915, plus a few decoded fields; it contains no EDID and no mode
 * list.  panel_width_mm/panel_height_mm are 0 until M3c reads the EDID.
 */
struct xg_kms_connector_info {
	unsigned int	size;			/*  0: sizeof(struct xg_kms_connector_info) */
	unsigned int	index;			/*  1: connector index (nub/child index) */
	unsigned int	child_index;		/*  2: VBT child_device_config index */
	unsigned int	handle;			/*  3: raw child handle (u16) */
	unsigned int	device_type;		/*  4: raw device_type (u16) */
	unsigned int	extended_type;		/*  5: raw extended_type (u16) */
	unsigned int	type;			/*  6: XG_KMS_CONN_* */
	unsigned int	port;			/*  7: raw dvo_port */
	unsigned int	aux_channel;		/*  8: raw aux_channel byte */
	unsigned int	ddc_pin;		/*  9: raw ddc_pin */
	unsigned int	i2c_pin;		/* 10: raw i2c_pin */
	unsigned int	target_addr;		/* 11: raw target_addr */
	unsigned int	dp_support;		/* 12: 0/1 */
	unsigned int	hdmi_support;		/* 13: 0/1 */
	unsigned int	tmds_support;		/* 14: 0/1 */
	unsigned int	dp_max_lane_count;	/* 15: raw VBT encoding, +1 = lanes */
	unsigned int	dp_max_link_rate;	/* 16: raw VBT index 0..7 */
	unsigned int	efp_routed;		/* 17: 0/1 */
	unsigned int	lane_reversal;		/* 18: 0/1 */
	unsigned int	lspcon;			/* 19: 0/1 */
	unsigned int	iboost;			/* 20: 0/1 */
	unsigned int	hpd_invert;		/* 21: 0/1 */
	unsigned int	use_vbt_vswing;		/* 22: 0/1 */
	unsigned int	dp_gpio_index;		/* 23: raw */
	unsigned int	dp_gpio_pin_num;	/* 24: raw u16 */
	unsigned int	dp_usb_type_c;		/* 25: 0/1 */
	unsigned int	tbt;			/* 26: 0/1 */
	unsigned int	dp_port_trace_length;	/* 27: raw nibble */
	unsigned int	efp_index;		/* 28: raw */
	unsigned int	pipe_cap;		/* 29: raw 2-bit */
	unsigned int	hpd_status;		/* 30: raw 2-bit */
	unsigned int	integrated_encoder;	/* 31: 0/1 */
	unsigned int	dvo_wiring;		/* 32: raw */
	unsigned int	dvo_function;		/* 33: raw */
	unsigned int	panel_width_mm;		/* 34: 0 = unknown */
	unsigned int	panel_height_mm;	/* 35: 0 = unknown */
	unsigned int	flags;			/* 36: XG_KMS_CONN_FLAG_* */
	unsigned int	reserved[9];		/* 37..45: must be 0 */
};

_Static_assert(sizeof(struct xg_kms_connector_info) == XG_KMS_CONNECTOR_BYTES,
    "xg_kms_connector_info must stay 46 u32 words");

typedef void (*xg_kms_vblank_proc)(void *arg, unsigned long long count,
	    unsigned int delta_us);

/*
 * Function table v1.  The first four u32 words are the header; the pointer
 * block is 8-byte aligned on x86_64.  Slots that are NULL are not part of v1.
 */
struct xg_kms_kapi {
	unsigned int	magic;			/* XG_KMS_KAPI_MAGIC */
	unsigned int	version;		/* XG_KMS_KAPI_VERSION */
	unsigned int	size;			/* sizeof(struct xg_kms_kapi) */
	unsigned int	flags;			/* XG_KMS_KAPI_FLAG_READ_ONLY */
	unsigned int	(*get_timing)(void *ctx, unsigned int fb_index,
			    unsigned int *out13);
	unsigned int	(*get_connector_info)(void *ctx, unsigned int index,
			    struct xg_kms_connector_info *out);
	unsigned int	(*get_edid)(void *ctx, unsigned int index, void *buf,
			    unsigned int buf_size, unsigned int *out_size);
	unsigned int	(*set_mode)(void *ctx, unsigned int fb_index,
			    unsigned int mode_id);
	unsigned int	(*flip)(void *ctx, unsigned int fb_index,
			    unsigned long long surface_id,
			    unsigned long long cookie);
	unsigned int	(*vblank)(void *ctx, unsigned int fb_index,
			    xg_kms_vblank_proc proc, void *arg);
	/*
	 * v1 requires this area to be zero.  M3d reuses words 0..2 as the
	 * additive scanout extension (XG_KMS_KAPI_FLAG_SCANOUT): alloc, free
	 * and get respectively, at offsets 64, 72 and 80.  Words 3..7 stay 0,
	 * the table stays 136 bytes and version 1 readers ignore it.
	 */
	unsigned long long reserved[8];
	void		*ctx;			/* NULL in v1 */
};

_Static_assert(sizeof(struct xg_kms_kapi) == XG_KMS_KAPI_SIZE,
    "xg_kms_kapi must stay 136 bytes");
_Static_assert(__builtin_offsetof(struct xg_kms_kapi, reserved) == 64,
    "scanout extension must start at reserved[0] (offset 64)");
_Static_assert(__builtin_offsetof(struct xg_kms_kapi, ctx) == 128,
    "table must stay 136 bytes");

static unsigned int	gXgMeasuredTiming[XG_KMS_TIMING_WORDS];
static int		gXgMeasuredTimingValid;

static int
xg_kms_timing_ok(const unsigned int *t)
{
	if (t[0] != XG_KMS_TIMING_TAG)
		return (0);
	if (t[1] == 0 || t[2] == 0 || t[6] == 0)
		return (0);
	if (t[3] < t[2] || t[4] < t[3] || t[5] < t[4])
		return (0);
	if (t[7] < t[6] || t[8] < t[7] || t[9] < t[8])
		return (0);
	return (1);
}

/*
 * Called once from xg_display_nub_publish() with the bytes of the
 * "XeGfxMeasuredPanelTiming" OSData (52 B).  The writer runs before the nub is
 * registered, so readers cannot observe a half-copied array through IOKit; no
 * lock is needed for this one-shot publish-then-read pattern.
 */
void
xg_display_set_measured_timing(const void *data, unsigned int len)
{
	unsigned int t[XG_KMS_TIMING_WORDS];

	if (data == NULL || len != (unsigned int)sizeof(t))
		return;
	memcpy(t, data, sizeof(t));
	if (!xg_kms_timing_ok(t)) {
		gXgMeasuredTimingValid = 0;
		return;
	}
	memcpy(gXgMeasuredTiming, t, sizeof(t));
	gXgMeasuredTimingValid = 1;
}

static unsigned int
xg_kms_get_timing(void *ctx, unsigned int fb_index, unsigned int *out13)
{
	(void)ctx;
	if (out13 == NULL)
		return (XG_KMS_ERR_BAD_ARGUMENT);
	if (fb_index >= XG_KMS_FIXED_MODE_BASE && fb_index < XG_KMS_FIXED_MODE_BASE + XG_KMS_MAX_FIXED_MODES)
		return (xg_kms_fixed_mode_timing(fb_index - XG_KMS_FIXED_MODE_BASE, out13));
	if (fb_index == XG_KMS_BOOT_STATE_INDEX)
		return (xg_kms_boot_state(out13));
	if (fb_index != 0)
		return (XG_KMS_ERR_NOT_FOUND);
	if (xg_display_external_dp())
		return xg_kms_current_timing(out13);
	if (!gXgMeasuredTimingValid)
		return (XG_KMS_ERR_NOT_READY);
	memcpy(out13, gXgMeasuredTiming, sizeof(gXgMeasuredTiming));
	return (XG_KMS_OK);
}

static unsigned int
xg_kms_vbt_u8(const unsigned char *c, unsigned int off)
{
	return ((unsigned int)c[off]);
}

static unsigned int
xg_kms_vbt_u16(const unsigned char *c, unsigned int off)
{
	return ((unsigned int)c[off] | ((unsigned int)c[off + 1] << 8));
}

int xg_display_keep_dp_child(unsigned int handle, unsigned int port)
{
	const unsigned char *c;

	if (gXgDpChildIndex < 0)
		return 0;
	c = gXgChildData + gXgDpChildIndex * gXgChildStride;
	return ((unsigned int)c[0] | ((unsigned int)c[1] << 8)) == handle && c[16] == port;
}

static unsigned int
xg_kms_vbt_bit(const unsigned char *c, unsigned int off, unsigned int bit)
{
	return (((unsigned int)c[off] >> bit) & 1U);
}

/*
 * Heuristic mapping of the 16-bit VBT device_type to the small connector enum.
 * The raw device_type is published as well, so M3c can refine this without an
 * ABI change.  Order matters: eDP/eDP-like internal panels first, then DSI,
 * HDMI/TMDS, plain DP.
 */
static unsigned int
xg_kms_conn_type(unsigned int device_type)
{
	if (device_type == 0x78c6U ||		/* DEVICE_TYPE_eDP */
	    ((device_type & (1U << 12)) &&	/* internal connector */
	     (device_type & (1U << 2))))	/* + DisplayPort */
		return (XG_KMS_CONN_EDP);
	if (device_type & (1U << 10))		/* MIPI output */
		return (XG_KMS_CONN_DSI);
	if (device_type == 0x60d2U ||		/* DEVICE_TYPE_HDMI */
	    (device_type & (1U << 4)))		/* TMDS/DVI signaling */
		return (XG_KMS_CONN_HDMI);
	if (device_type & (1U << 2))		/* DisplayPort output */
		return (XG_KMS_CONN_DP);
	return (XG_KMS_CONN_UNKNOWN);
}

/*
 * Static connector 0 (and, for M3c, 1..4) information from the raw
 * child_device_config bytes captured by intel_bios.c.  Field offsets are the
 * packed VBT layout from intel_vbt_defs.h (40 B total, see docs/11-M3B-KAPI.md).
 */
static unsigned int
xg_kms_get_connector_info(void *ctx, unsigned int index,
    struct xg_kms_connector_info *out)
{
	const unsigned char *c;
	struct xg_kms_connector_info info = { .size = sizeof(info) };
	unsigned int dt;
	unsigned int raw_index = index;
	unsigned int flags = 0;

	(void)ctx;
	if (out == NULL || out->size != sizeof(*out))
		return (XG_KMS_ERR_BAD_ARGUMENT);
	if (gXgChildCount <= 0 || gXgChildSize < XG_KMS_CHILD_BYTES)
		return (XG_KMS_ERR_NOT_READY);
	if (xg_display_external_dp()) {
		if (index != 0)
			return (XG_KMS_ERR_NOT_FOUND);
		if (gXgDpChildIndex < 0)
			return (XG_KMS_ERR_NOT_READY);
		raw_index = (unsigned int)gXgDpChildIndex;
	}
	if (raw_index >= (unsigned int)gXgChildCount)
		return (XG_KMS_ERR_NOT_FOUND);

	c = gXgChildData + raw_index * gXgChildStride;

	info.index = index;
	info.child_index = raw_index;
	info.handle = xg_kms_vbt_u16(c, 0);
	dt = xg_kms_vbt_u16(c, 2);
	info.device_type = dt;
	info.extended_type = xg_kms_vbt_u16(c, 30);
	info.type = xg_display_external_dp() ? XG_KMS_CONN_DP : xg_kms_conn_type(dt);
	info.port = xg_kms_vbt_u8(c, 16);
	info.i2c_pin = xg_kms_vbt_u8(c, 17);
	info.target_addr = xg_kms_vbt_u8(c, 18);
	info.ddc_pin = xg_kms_vbt_u8(c, 19);
	info.efp_routed = xg_kms_vbt_bit(c, 23, 0);
	info.lane_reversal = xg_kms_vbt_bit(c, 23, 1);
	info.lspcon = xg_kms_vbt_bit(c, 23, 2);
	info.iboost = xg_kms_vbt_bit(c, 23, 3);
	info.hpd_invert = xg_kms_vbt_bit(c, 23, 4);
	info.use_vbt_vswing = xg_kms_vbt_bit(c, 23, 5);
	info.dp_max_lane_count = ((unsigned int)c[23] >> 6) & 0x3U;
	info.hdmi_support = xg_kms_vbt_bit(c, 24, 0);
	info.dp_support = xg_kms_vbt_bit(c, 24, 1);
	info.tmds_support = xg_kms_vbt_bit(c, 24, 2);
	info.aux_channel = xg_kms_vbt_u8(c, 25);
	info.pipe_cap = (unsigned int)c[27] & 0x3U;
	info.hpd_status = ((unsigned int)c[27] >> 3) & 0x3U;
	info.integrated_encoder = xg_kms_vbt_bit(c, 27, 5);
	info.dvo_wiring = xg_kms_vbt_u8(c, 28);
	info.dvo_function = xg_kms_vbt_u8(c, 32);
	info.dp_usb_type_c = xg_kms_vbt_bit(c, 33, 0);
	info.tbt = xg_kms_vbt_bit(c, 33, 1);
	info.dp_port_trace_length = ((unsigned int)c[33] >> 4) & 0xfU;
	info.dp_gpio_index = xg_kms_vbt_u8(c, 34);
	info.dp_gpio_pin_num = xg_kms_vbt_u16(c, 35);
	info.dp_max_link_rate = (unsigned int)c[38] & 0x7U;
	info.efp_index = xg_kms_vbt_u8(c, 39);
	info.panel_width_mm = 0;	/* M3c: from EDID */
	info.panel_height_mm = 0;	/* M3c: from EDID */

	if (info.type == XG_KMS_CONN_EDP || (dt & (1U << 12)))
		flags |= XG_KMS_CONN_FLAG_INTERNAL;
	if (info.type == XG_KMS_CONN_EDP || info.type == XG_KMS_CONN_DP ||
	    info.dp_support || (dt & (1U << 2)))
		flags |= XG_KMS_CONN_FLAG_DP;
	if (info.type == XG_KMS_CONN_HDMI || info.hdmi_support ||
	    info.tmds_support)
		flags |= XG_KMS_CONN_FLAG_HDMI;
	if (info.aux_channel != 0)
		flags |= XG_KMS_CONN_FLAG_AUX;
	if (info.lspcon)
		flags |= XG_KMS_CONN_FLAG_LSPCON;
	if (info.efp_routed)
		flags |= XG_KMS_CONN_FLAG_EFP_ROUTED;
	info.flags = flags;

	memcpy(out, &info, sizeof(info));
	return (XG_KMS_OK);
}

/*
 * ============================================================================
 * M3d: scanout buffers (additive v1.1 extension in reserved[0..2])
 * ============================================================================
 *
 * These are the only KAPI entry points that allocate or take a lock, so they
 * are thread-context only: allocScanout/freeScanout refuse to run with
 * preemption disabled or interrupts off (holding a spin lock is not a
 * sleepable context), and all three are serialized by gXgScanoutLock.  The
 * buffer itself is physically contiguous IOKit memory below 4 GiB: the
 * adapter's xgGetScanoutInfo hands phys to the accel as a u32, and the
 * display's PLANE_SURF wants a 32-bit GGTT offset.  The GGTT range is filled
 * with I915_CACHE_NONE PTEs by xg_scanout_ggtt_map(), so a future i915 plane
 * can point at it without a GEM object.
 *
 * No display register is written here.  Callers must stop scanning out of a
 * buffer before freeScanout: freeing the backing memory while a plane still
 * points at the GGTT range would let the display read freed pages.
 */

_Static_assert(sizeof(struct xg_kms_scanout_info) == XG_KMS_SCANOUT_BYTES,
    "xg_kms_scanout_info must stay 48 bytes");
_Static_assert(__builtin_offsetof(struct xg_kms_scanout_info, kva) == 8,
    "kva must stay at offset 8");
_Static_assert(__builtin_offsetof(struct xg_kms_scanout_info, pitch) == 24,
    "pitch must stay at offset 24");
_Static_assert(__builtin_offsetof(struct xg_kms_scanout_info, surface_id) == 40,
    "surfaceId must stay at offset 40");
_Static_assert(__builtin_offsetof(struct xg_kms_scanout_info, reserved) == 44,
    "reserved must stay at offset 44");

_Static_assert(XG_KMS_SCANOUT_MAX == XG_SCANOUT_GGTT_MAX,
    "KAPI scanout limit and GGTT slot pool must agree");

#define XG_KMS_SCANOUT_HANDLE(gen, slot)	(((gen) << 8) | ((slot) + 1U))
#define XG_KMS_SCANOUT_HANDLE_SLOT(h)		(((h) & 0xffU) - 1U)

struct xg_kms_scanout {
	unsigned int			used;
	unsigned int			handle;
	unsigned int			last_handle;	/* for idempotent free */
	unsigned long long		ggtt;
	unsigned long long		size;
	xg_vm_xmem_t			mem;
	struct xg_kms_scanout_info	info;
};

static struct xg_kms_scanout	gXgScanout[XG_KMS_SCANOUT_MAX];
static unsigned int		gXgScanoutGeneration;
static DEFINE_MUTEX(gXgScanoutLock);

static int
xg_kms_scanout_thread_ok(void)
{
	return (preemption_enabled() && ml_get_interrupts_enabled());
}

static unsigned int
xg_kms_alloc_scanout(void *ctx, unsigned int width, unsigned int height,
    unsigned int bytes_per_pixel, unsigned int flags,
    struct xg_kms_scanout_info *out)
{
	unsigned long long row, pitch, size, pa = 0;
	void *kva = NULL;
	xg_vm_xmem_t mem;
	unsigned int slot, gen, handle;
	int err;

	(void)ctx;
	if (out == NULL || width == 0 || height == 0)
		return (XG_KMS_ERR_BAD_ARGUMENT);
	if ((flags & ~XG_KMS_SCANOUT_FLAG_ZERO) != 0)
		return (XG_KMS_ERR_UNSUPPORTED);
	if (bytes_per_pixel != 4)	/* 32 bpp is the only format M3d consumes */
		return (XG_KMS_ERR_UNSUPPORTED);
	if (width > XG_KMS_SCANOUT_MAX_WIDTH || height > XG_KMS_SCANOUT_MAX_HEIGHT)
		return (XG_KMS_ERR_BAD_ARGUMENT);

	row = (unsigned long long)width * bytes_per_pixel;
	pitch = (row + 255ULL) & ~255ULL;	/* Xe display minimum stride alignment */
	if (pitch > 0xffffffffULL)
		return (XG_KMS_ERR_BAD_ARGUMENT);
	size = pitch * height;
	size = (size + 4095ULL) & ~4095ULL;
	if (size > XG_KMS_SCANOUT_MAX_BYTES)
		return (XG_KMS_ERR_BAD_ARGUMENT);
	if (!xg_kms_scanout_thread_ok())
		return (XG_KMS_ERR_NOT_READY);

	mutex_lock(&gXgScanoutLock);
	for (slot = 0; slot < XG_KMS_SCANOUT_MAX; slot++)
		if (!gXgScanout[slot].used)
			break;
	if (slot == XG_KMS_SCANOUT_MAX) {
		mutex_unlock(&gXgScanoutLock);
		return (XG_KMS_ERR_BUSY);
	}

	mem = xg_vm_x_alloc(size, 0x00000000fffff000ULL, XG_VM_CACHE_WC, &kva, &pa);
	if (mem == NULL || kva == NULL || pa == 0) {
		if (mem != NULL)
			xg_vm_x_release(mem);
		mutex_unlock(&gXgScanoutLock);
		pr_err("XEGFX_KAPI_SCANOUT: alloc %ux%u bpp %u: no contiguous memory (%llu bytes)\n",
		    width, height, bytes_per_pixel * 8, size);
		return (XG_KMS_ERR_NO_MEMORY);
	}
	if (pa + size > 0x100000000ULL) {
		xg_vm_x_release(mem);
		mutex_unlock(&gXgScanoutLock);
		pr_err("XEGFX_KAPI_SCANOUT: alloc %ux%u: physical 0x%llx is not below 4 GiB\n",
		    width, height, pa);
		return (XG_KMS_ERR_NO_MEMORY);
	}

	/* never expose stale kernel memory to the display */
	memset(kva, 0, (size_t)size);
	xg_vm_x_flush(kva, size);
	__sync_synchronize();

	err = xg_scanout_ggtt_map(size, pa, &gXgScanout[slot].ggtt);
	if (err != 0) {
		xg_vm_x_release(mem);
		mutex_unlock(&gXgScanoutLock);
		pr_err("XEGFX_KAPI_SCANOUT: alloc %ux%u: GGTT map failed (%d)\n", width, height, err);
		return (err == -ENODEV ? XG_KMS_ERR_NOT_READY :
		    (err == -ENOSPC ? XG_KMS_ERR_NO_MEMORY : XG_KMS_ERR_UNSUPPORTED));
	}

	gXgScanoutGeneration = (gXgScanoutGeneration + 1U) & 0xffffffU;
	if (gXgScanoutGeneration == 0)
		gXgScanoutGeneration = 1;
	gen = gXgScanoutGeneration;
	handle = XG_KMS_SCANOUT_HANDLE(gen, slot);

	gXgScanout[slot].size = size;
	gXgScanout[slot].mem = mem;
	gXgScanout[slot].handle = handle;
	gXgScanout[slot].last_handle = 0;
	gXgScanout[slot].info.phys = pa;
	gXgScanout[slot].info.kva = kva;
	gXgScanout[slot].info.length = size;
	gXgScanout[slot].info.pitch = (unsigned int)pitch;
	gXgScanout[slot].info.width = width;
	gXgScanout[slot].info.height = height;
	gXgScanout[slot].info.bpp = bytes_per_pixel * 8;
	gXgScanout[slot].info.surface_id = handle;
	gXgScanout[slot].info.reserved = (unsigned int)gXgScanout[slot].ggtt;
	gXgScanout[slot].used = 1;
	*out = gXgScanout[slot].info;

	pr_info("XEGFX_KAPI_SCANOUT: alloc handle=0x%x slot=%u %ux%u pitch=%u size=%llu phys=0x%llx ggtt=0x%llx\n",
	    handle, slot, width, height, (unsigned int)pitch, size, pa, gXgScanout[slot].ggtt);
	mutex_unlock(&gXgScanoutLock);
	return (XG_KMS_OK);
}

static unsigned int
xg_kms_get_scanout_info(void *ctx, unsigned int handle, struct xg_kms_scanout_info *out)
{
	unsigned int slot;

	(void)ctx;
	if (out == NULL || handle == 0 || (handle & 0xffU) == 0)
		return (XG_KMS_ERR_BAD_ARGUMENT);
	slot = XG_KMS_SCANOUT_HANDLE_SLOT(handle);
	if (slot >= XG_KMS_SCANOUT_MAX)
		return (XG_KMS_ERR_NOT_FOUND);
	if (!xg_kms_scanout_thread_ok())
		return (XG_KMS_ERR_NOT_READY);

	mutex_lock(&gXgScanoutLock);
	if (!gXgScanout[slot].used || gXgScanout[slot].handle != handle) {
		mutex_unlock(&gXgScanoutLock);
		return (XG_KMS_ERR_NOT_FOUND);
	}
	*out = gXgScanout[slot].info;
	mutex_unlock(&gXgScanoutLock);
	return (XG_KMS_OK);
}

static unsigned int
xg_kms_free_scanout(void *ctx, unsigned int handle)
{
	unsigned int slot;
	int err;

	(void)ctx;
	if (handle == 0)	/* NULL handle: nothing to do */
		return (XG_KMS_OK);
	if ((handle & 0xffU) == 0)
		return (XG_KMS_ERR_BAD_ARGUMENT);
	slot = XG_KMS_SCANOUT_HANDLE_SLOT(handle);
	if (slot >= XG_KMS_SCANOUT_MAX)
		return (XG_KMS_ERR_BAD_ARGUMENT);
	if (!xg_kms_scanout_thread_ok())
		return (XG_KMS_ERR_NOT_READY);

	mutex_lock(&gXgScanoutLock);
	if (!gXgScanout[slot].used) {
		unsigned int rc = (gXgScanout[slot].last_handle == handle) ?
		    XG_KMS_OK : XG_KMS_ERR_NOT_FOUND;

		mutex_unlock(&gXgScanoutLock);
		return (rc);	/* repeated free of this handle is safe */
	}
	if (gXgScanout[slot].handle != handle) {
		mutex_unlock(&gXgScanoutLock);
		return (XG_KMS_ERR_NOT_FOUND);	/* stale handle for a live buffer */
	}

	err = xg_scanout_ggtt_unmap(gXgScanout[slot].ggtt, gXgScanout[slot].size);
	if (err != 0) {
		mutex_unlock(&gXgScanoutLock);
		pr_err("XEGFX_KAPI_SCANOUT: free handle=0x%x: GGTT unmap failed (%d), buffer kept\n",
		    handle, err);
		return (XG_KMS_ERR_NOT_READY);
	}
	xg_vm_x_release(gXgScanout[slot].mem);
	gXgScanout[slot].last_handle = handle;
	gXgScanout[slot].mem = NULL;
	gXgScanout[slot].ggtt = 0;
	gXgScanout[slot].size = 0;
	gXgScanout[slot].handle = 0;
	gXgScanout[slot].used = 0;
	memset(&gXgScanout[slot].info, 0, sizeof(gXgScanout[slot].info));
	mutex_unlock(&gXgScanoutLock);

	pr_info("XEGFX_KAPI_SCANOUT: free handle=0x%x slot=%u\n", handle, slot);
	return (XG_KMS_OK);
}

static const struct xg_kms_kapi gXgKmsKapi = {
	.magic = XG_KMS_KAPI_MAGIC,
	.version = XG_KMS_KAPI_VERSION,
	.size = sizeof(struct xg_kms_kapi),
	.flags = XG_KMS_KAPI_FLAG_READ_ONLY | XG_KMS_KAPI_FLAG_SCANOUT | XG_KMS_KAPI_FLAG_MODESET |
	    XG_KMS_KAPI_FLAG_PLANE | XG_KMS_KAPI_FLAG_BACKLIGHT | XG_KMS_KAPI_FLAG_VBLANK,
	.get_timing = xg_kms_get_timing,
	.get_connector_info = xg_kms_get_connector_info,
	.get_edid = NULL,		/* M3c */
	.set_mode = xg_kms_set_mode,	/* K3 */
	.flip = NULL,			/* M3c */
	.vblank = NULL,			/* M3c */
	.reserved = {
		(unsigned long long)xg_kms_alloc_scanout,
		(unsigned long long)xg_kms_free_scanout,
		(unsigned long long)xg_kms_get_scanout_info,
		(unsigned long long)xg_kms_set_plane,		/* A1 */
		(unsigned long long)xg_kms_backlight,		/* B1 */
		(unsigned long long)xg_kms_wait_vblank,		/* E3 */
		(unsigned long long)xg_kms_cursor_image,	/* C1: no flag bit -- an older adapter must keep accepting this table */
		(unsigned long long)xg_kms_cursor_move,		/* C1 */
	},
	.ctx = NULL,
};

const void *
xg_display_kms_kapi(void)
{
	return ((const void *)&gXgKmsKapi);
}

unsigned int
xg_display_kms_kapi_size(void)
{
	return ((unsigned int)sizeof(gXgKmsKapi));
}

unsigned int
xg_display_kms_kapi_version(void)
{
	return (XG_KMS_KAPI_VERSION);
}

/*
 * M3d diagnostic: exercise the scanout extension through the published table
 * (write debug.xegfx_kms_scanout).  It allocates only when explicitly called;
 * the default boot path never reaches this function.  The 64x64 buffer keeps
 * the test cheap while still covering allocation, GGTT mapping, get and both
 * the first and the repeated free.
 */
typedef unsigned int (*xg_kms_alloc_scanout_fn)(void *, unsigned int, unsigned int,
    unsigned int, unsigned int, struct xg_kms_scanout_info *);
typedef unsigned int (*xg_kms_free_scanout_fn)(void *, unsigned int);
typedef unsigned int (*xg_kms_get_scanout_info_fn)(void *, unsigned int,
    struct xg_kms_scanout_info *);

int
xg_display_kms_scanout_selftest(char *out, int max)
{
	struct xg_kms_scanout_info a, b, c;
	xg_kms_alloc_scanout_fn alloc;
	xg_kms_free_scanout_fn free_fn;
	xg_kms_get_scanout_info_fn get;
	unsigned int rc_alloc, rc_get, rc_free, rc_free2, rc_realloc, rc_cleanup;
	int alloc_ok, get_ok, free_ok, free2_ok, realloc_ok;

	if (out == NULL || max <= 0)
		return (0);
	if ((gXgKmsKapi.flags & XG_KMS_KAPI_FLAG_SCANOUT) == 0 ||
	    gXgKmsKapi.reserved[0] == 0 || gXgKmsKapi.reserved[1] == 0 ||
	    gXgKmsKapi.reserved[2] == 0) {
		snprintf(out, (size_t)max, "XEGFX_KAPI_SCANOUT selftest: extension not present\n");
		return ((int)strlen(out));
	}
	memset(&a, 0, sizeof(a));
	memset(&b, 0, sizeof(b));
	memset(&c, 0, sizeof(c));
	alloc = (xg_kms_alloc_scanout_fn)(unsigned long long)gXgKmsKapi.reserved[0];
	free_fn = (xg_kms_free_scanout_fn)(unsigned long long)gXgKmsKapi.reserved[1];
	get = (xg_kms_get_scanout_info_fn)(unsigned long long)gXgKmsKapi.reserved[2];

	rc_alloc = alloc(NULL, 64, 64, 4, 0, &a);
	alloc_ok = rc_alloc == XG_KMS_OK && a.phys != 0 && a.kva != NULL &&
	    a.length >= 16384 && a.pitch == 256 && a.width == 64 && a.height == 64 &&
	    a.bpp == 32 && a.surface_id != 0 && a.reserved != 0;
	if (alloc_ok) {
		rc_get = get(NULL, a.surface_id, &b);
		get_ok = rc_get == XG_KMS_OK && b.phys == a.phys && b.kva == a.kva &&
		    b.length == a.length && b.pitch == a.pitch && b.width == a.width &&
		    b.height == a.height && b.bpp == a.bpp &&
		    b.surface_id == a.surface_id && b.reserved == a.reserved;
		rc_free = free_fn(NULL, a.surface_id);
		rc_free2 = free_fn(NULL, a.surface_id);
		free_ok = rc_free == XG_KMS_OK;
		free2_ok = rc_free2 == XG_KMS_OK;
		rc_realloc = alloc(NULL, 64, 64, 4, 0, &c);
		realloc_ok = rc_realloc == XG_KMS_OK && c.surface_id != a.surface_id;
		if (realloc_ok) {
			rc_cleanup = free_fn(NULL, c.surface_id);
			realloc_ok = rc_cleanup == XG_KMS_OK;
		}
	} else {
		rc_get = rc_free = rc_free2 = rc_realloc = XG_KMS_ERR_NOT_READY;
		rc_cleanup = XG_KMS_ERR_NOT_READY;
		get_ok = free_ok = free2_ok = realloc_ok = 0;
	}

	snprintf(out, (size_t)max,
	    "XEGFX_KAPI_SCANOUT selftest: flags=0x%x alloc=%u get=%u free=%u free2=%u realloc=%u "
	    "handle=0x%x phys=0x%llx pitch=%u len=%llu ggtt=0x%x result=%s\n",
	    gXgKmsKapi.flags, rc_alloc, rc_get, rc_free, rc_free2, rc_realloc,
	    a.surface_id, a.phys, a.pitch, a.length, a.reserved,
	    (alloc_ok && get_ok && free_ok && free2_ok && realloc_ok) ? "PASS" : "FAIL");
	return ((int)strlen(out));
}

/*
 * Panel identity over the internal panel's AUX channel, the way NVKMS hands NVRMFB its EDID: read-only DPCD and
 * EDID (I2C-over-AUX), captured once before the display nub is published and handed to the adapter as OSData.
 *
 * The i915 display engine is left alone (no encoders, no power-domain bookkeeping), so this talks to the AUX
 * registers directly with the sequence intel_dp_aux_xfer() uses on display version 14 (MTL/ARL): request the AUX
 * power (XELPDP_DP_AUX_CH_CTL_POWER_REQUEST, fixed 600 us), keep that bit set in every send, poll DONE instead of
 * taking the AUX interrupt, clear the sticky status bits after each message.  Only read transactions are sent to
 * the sink; nothing touches the link, the pipe or the panel power sequencer.  It refuses to run while PSR owns the
 * AUX channel, and any failure leaves the capture empty (the adapter then reports no EDID, as before).
 */
#include <linux/delay.h>
#include "intel_de.h"
#include "i915_reg.h"
#include "intel_dp_aux_regs.h"

#define XG_AUX_PSR_CTL		0x60800		/* EDP_PSR_CTL(TRANSCODER_A), EDP_PSR_ENABLE = bit 31 */
#define XG_AUX_PSR2_CTL		0x60900		/* EDP_PSR2_CTL(TRANSCODER_A), EDP_PSR2_ENABLE = bit 31 */
#define XG_AUX_REPLY_ACK	0x0
#define XG_AUX_REPLY_DEFER	0x2
#define XG_AUX_NATIVE_READ	0x9
#define XG_AUX_I2C_WRITE	0x0
#define XG_AUX_I2C_READ		0x1
#define XG_AUX_I2C_MOT		0x4
#define XG_DDC_ADDR		0x50
#define XG_DDC_SEGMENT_ADDR	0x30

static struct intel_display	*gXgAuxDisplay;
static unsigned int		gXgAuxCtl;		/* AUX_CH_CTL offset; data registers follow at +4 */
static unsigned char		gXgEdid[XG_DISPLAY_MAX_EDID];
static unsigned int		gXgEdidLen;
static unsigned char		gXgDpcd[XG_DISPLAY_DPCD_BYTES];
static int			gXgDpcdOk;
static char			gXgAuxWhy[128] = "not run";

void
xg_display_aux_set_display(struct intel_display *display)
{
	gXgAuxDisplay = display;
}

static u32
xg_aux_rd(unsigned int off)
{
	return (intel_de_read(gXgAuxDisplay, _MMIO(off)));
}

static void
xg_aux_wr(unsigned int off, u32 v)
{
	intel_de_write(gXgAuxDisplay, _MMIO(off), v);
}

/* one AUX message: send <= 20 bytes, receive <= 20; returns the received byte count or -errno */
static int
xg_aux_xfer(const u8 *tx, int txn, u8 *rx, int rxmax)
{
	u32 status = 0, send;
	int i, j, try, wait, n;

	for (try = 0; try < 3; try++) {
		status = xg_aux_rd(gXgAuxCtl);
		if ((status & DP_AUX_CH_CTL_SEND_BUSY) == 0)
			break;
		msleep(1);
	}
	if (status & DP_AUX_CH_CTL_SEND_BUSY)
		return (-EBUSY);
	send = DP_AUX_CH_CTL_SEND_BUSY | DP_AUX_CH_CTL_DONE | DP_AUX_CH_CTL_TIME_OUT_ERROR |
	    DP_AUX_CH_CTL_TIME_OUT_MAX | DP_AUX_CH_CTL_RECEIVE_ERROR | DP_AUX_CH_CTL_MESSAGE_SIZE(txn) |
	    DP_AUX_CH_CTL_FW_SYNC_PULSE_SKL(18) | DP_AUX_CH_CTL_SYNC_PULSE_SKL(32) |
	    XELPDP_DP_AUX_CH_CTL_POWER_REQUEST;
	for (try = 0; try < 5; try++) {
		for (i = 0; i < txn; i += 4) {
			u32 v = 0;
			for (j = 0; j < 4 && i + j < txn; j++)
				v |= (u32)tx[i + j] << ((3 - j) * 8);
			xg_aux_wr(gXgAuxCtl + 4 + i, v);
		}
		xg_aux_wr(gXgAuxCtl, send);
		for (wait = 0; wait < 1000; wait++) {		/* 10 ms, as intel_dp_aux_wait_done() */
			status = xg_aux_rd(gXgAuxCtl);
			if ((status & DP_AUX_CH_CTL_SEND_BUSY) == 0)
				break;
			udelay(10);
		}
		xg_aux_wr(gXgAuxCtl, status | DP_AUX_CH_CTL_DONE | DP_AUX_CH_CTL_TIME_OUT_ERROR |
		    DP_AUX_CH_CTL_RECEIVE_ERROR);
		if (status & DP_AUX_CH_CTL_SEND_BUSY)
			return (-ETIMEDOUT);
		if (status & DP_AUX_CH_CTL_TIME_OUT_ERROR)
			continue;
		if (status & DP_AUX_CH_CTL_RECEIVE_ERROR) {
			usleep_range(400, 500);
			continue;
		}
		if (status & DP_AUX_CH_CTL_DONE)
			break;
	}
	if ((status & DP_AUX_CH_CTL_DONE) == 0 || (status & (DP_AUX_CH_CTL_TIME_OUT_ERROR | DP_AUX_CH_CTL_RECEIVE_ERROR)))
		return (-EIO);
	n = REG_FIELD_GET(DP_AUX_CH_CTL_MESSAGE_SIZE_MASK, status);
	if (n == 0 || n > 20)
		return (-EIO);
	if (n > rxmax)
		n = rxmax;
	for (i = 0; i < n; i += 4) {
		u32 v = xg_aux_rd(gXgAuxCtl + 4 + i);
		for (j = 0; j < 4 && i + j < n; j++)
			rx[i + j] = (u8)(v >> ((3 - j) * 8));
	}
	return (n);
}

/* native DPCD read of up to 16 bytes, with the DEFER retries drm_dp_dpcd_access() does */
static int
xg_aux_dpcd_read(unsigned int addr, u8 *buf, int len)
{
	u8 tx[4] = { (u8)((XG_AUX_NATIVE_READ << 4) | ((addr >> 16) & 0xf)), (u8)(addr >> 8), (u8)addr, (u8)(len - 1) };
	u8 rx[20];
	int try, n;

	for (try = 0; try < 32; try++) {
		n = xg_aux_xfer(tx, sizeof(tx), rx, 1 + len);
		if (n < 0)
			return (n);
		if (((rx[0] >> 4) & 0x3) == XG_AUX_REPLY_DEFER) {
			usleep_range(400, 500);
			continue;
		}
		if (((rx[0] >> 4) & 0x3) != XG_AUX_REPLY_ACK || n - 1 < len)
			return (-EIO);
		memcpy(buf, rx + 1, len);
		return (len);
	}
	return (-EAGAIN);
}

/* one I2C-over-AUX message; data/len only for writes, len bytes back for reads; returns bytes moved or -errno */
static int
xg_aux_i2c(u8 req, u8 i2c_addr, const u8 *wdata, u8 *rdata, int len)
{
	u8 tx[5], rx[20];
	int try, n, txn;

	tx[0] = (u8)(req << 4);
	tx[1] = 0;
	tx[2] = i2c_addr;
	if (len == 0) {
		txn = 3;				/* address-only: start/stop the I2C transaction */
	} else {
		tx[3] = (u8)(len - 1);
		txn = 4;
		if (wdata != NULL) {
			tx[4] = wdata[0];		/* writes here are a single offset/segment byte */
			txn = 5;
		}
	}
	for (try = 0; try < 32; try++) {
		n = xg_aux_xfer(tx, txn, rx, 1 + 16);
		if (n < 0)
			return (n);
		if (((rx[0] >> 4) & 0x3) == XG_AUX_REPLY_DEFER || ((rx[0] >> 6) & 0x3) == XG_AUX_REPLY_DEFER) {
			usleep_range(400, 500);
			continue;
		}
		if (((rx[0] >> 4) & 0x3) != XG_AUX_REPLY_ACK || ((rx[0] >> 6) & 0x3) != XG_AUX_REPLY_ACK)
			return (-EIO);
		if (rdata != NULL && len > 0) {
			if (n - 1 <= 0)
				continue;		/* a short reply carries no data yet: ask again */
			memcpy(rdata, rx + 1, n - 1);
			return (n - 1);
		}
		return (len);
	}
	return (-EAGAIN);
}

/* one 128-byte EDID block through E-DDC (segment pointer at 0x30 for blocks 2 and up) */
static int
xg_aux_edid_block(unsigned int block, u8 *out)
{
	u8 seg = (u8)(block / 2), off = (u8)((block % 2) * 128);
	int got = 0, n, rc = 0;

	if (seg != 0 && xg_aux_i2c(XG_AUX_I2C_WRITE | XG_AUX_I2C_MOT, XG_DDC_SEGMENT_ADDR, &seg, NULL, 1) < 0)
		rc = -EIO;
	if (rc == 0 && xg_aux_i2c(XG_AUX_I2C_WRITE | XG_AUX_I2C_MOT, XG_DDC_ADDR, &off, NULL, 1) < 0)
		rc = -EIO;
	while (rc == 0 && got < 128) {
		n = xg_aux_i2c(XG_AUX_I2C_READ | XG_AUX_I2C_MOT, XG_DDC_ADDR, NULL, out + got, 128 - got < 16 ? 128 - got : 16);
		if (n <= 0)
			rc = n < 0 ? n : -EIO;
		else
			got += n;
	}
	(void)xg_aux_i2c(XG_AUX_I2C_READ, XG_DDC_ADDR, NULL, NULL, 0);	/* stop, also after a failure */
	return (rc);
}

static int
xg_edid_block_ok(const u8 *b, int base)
{
	static const u8 hdr[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
	unsigned int i, sum = 0;

	if (base && memcmp(b, hdr, sizeof(hdr)) != 0)
		return (0);
	for (i = 0; i < 128; i++)
		sum += b[i];
	return ((sum & 0xff) == 0);
}

static int
xg_display_aux_probe_child(const unsigned char *c)
{
	static const unsigned int regions[XG_DISPLAY_DPCD_BYTES / 16] = { 0x000, 0x060, 0x070, 0x2200, 0x700 };
	char line[192];
	unsigned int i, aux = 0, ext;
	u32 ctl, psr, psr2;
	int rc;

	gXgEdidLen = 0;
	gXgDpcdOk = 0;
	if (gXgAuxDisplay == NULL) {
		snprintf(gXgAuxWhy, sizeof(gXgAuxWhy), "no display (VBT not parsed)");
		return (-ENODEV);
	}
	if (c != NULL)
		aux = xg_kms_vbt_u8(c, 25);
	if (aux == 0x40)		/* DP_AUX_A */
		gXgAuxCtl = 0x64010;
	else if (aux == 0x10)		/* DP_AUX_B */
		gXgAuxCtl = 0x64110;
	else if (xg_display_external_dp() && DISPLAY_VER(gXgAuxDisplay) == 14 &&
	    (aux == 0x60 || aux == 0x70 || aux == 0x80)) {
		enum aux_ch ch = aux == 0x60 ? AUX_CH_USBC1 : aux == 0x70 ? AUX_CH_USBC2 : AUX_CH_USBC3;
		gXgAuxCtl = i915_mmio_reg_offset(XELPDP_DP_AUX_CH_CTL(gXgAuxDisplay, ch));
		if (gXgAuxCtl != xg_dp_aux_ctl(aux)) {
			snprintf(gXgAuxWhy, sizeof(gXgAuxWhy), "AUX mapping mismatch for VBT 0x%x", aux);
			goto out;
		}
	}
	else {
		snprintf(gXgAuxWhy, sizeof(gXgAuxWhy), "display aux 0x%x not a supported display-v14 AUX channel (child %s)",
		    aux, c ? "found" : "missing");
		goto out;
	}
	psr = xg_aux_rd(XG_AUX_PSR_CTL);
	psr2 = xg_aux_rd(XG_AUX_PSR2_CTL);
	if (!xg_display_external_dp() && ((psr | psr2) & (1U << 31))) {
		snprintf(gXgAuxWhy, sizeof(gXgAuxWhy), "PSR owns the AUX channel (psr 0x%08x psr2 0x%08x): not read", psr, psr2);
		goto out;
	}
	ctl = xg_aux_rd(gXgAuxCtl);
	if (!(ctl & XELPDP_DP_AUX_CH_CTL_POWER_REQUEST)) {
		xg_aux_wr(gXgAuxCtl, ctl | XELPDP_DP_AUX_CH_CTL_POWER_REQUEST);
		usleep_range(600, 1200);	/* xelpdp_aux_power_well_enable(): the status bit is not reliable */
	}
	for (i = 0; i < XG_DISPLAY_DPCD_BYTES / 16; i++) {
		rc = xg_aux_dpcd_read(regions[i], gXgDpcd + i * 16, 16);
		if (rc < 0 && i == 0) {
			snprintf(gXgAuxWhy, sizeof(gXgAuxWhy), "DPCD 0x000 read failed (%d, ctl 0x%08x)", rc, xg_aux_rd(gXgAuxCtl));
			goto out;
		}
		if (rc < 0)
			memset(gXgDpcd + i * 16, 0, 16);	/* optional region: absent reads as zero */
	}
	gXgDpcdOk = 1;
	rc = xg_aux_edid_block(0, gXgEdid);
	if (rc < 0 || !xg_edid_block_ok(gXgEdid, 1)) {
		snprintf(gXgAuxWhy, sizeof(gXgAuxWhy), "EDID block 0 %s (%d)", rc < 0 ? "read failed" : "invalid", rc);
		goto out;
	}
	gXgEdidLen = 128;
	ext = gXgEdid[126];
	for (i = 1; i <= ext && (i + 1) * 128 <= sizeof(gXgEdid); i++) {
		if (xg_aux_edid_block(i, gXgEdid + i * 128) < 0 || !xg_edid_block_ok(gXgEdid + i * 128, 0))
			break;			/* keep the valid prefix */
		gXgEdidLen = (i + 1) * 128;
	}
	snprintf(gXgAuxWhy, sizeof(gXgAuxWhy), "ok: aux 0x%x ctl 0x%x, EDID %u of %u byte(s)", aux, gXgAuxCtl,
	    gXgEdidLen, (ext + 1) * 128);
out:
	snprintf(line, sizeof(line), "aux: %s", gXgAuxWhy);
	xg_disp_line(gXgAuxDisplay, line);
	if (gXgDpcdOk) {
		snprintf(line, sizeof(line), "dpcd: rev 0x%02x max_rate 0x%02x lanes 0x%02x edp 0x%02x dsc 0x%02x ext 0x%02x/0x%02x/0x%02x edp_cap 0x%02x/0x%02x/0x%02x",
		    gXgDpcd[0], gXgDpcd[1], gXgDpcd[2], gXgDpcd[0x0d], gXgDpcd[16], gXgDpcd[48], gXgDpcd[49], gXgDpcd[50],
		    gXgDpcd[64], gXgDpcd[65], gXgDpcd[66]);
		xg_disp_line(gXgAuxDisplay, line);
	}
	return (gXgEdidLen ? 0 : -EIO);
}

/* Probe only the SST port already driven by pipe A; other TC banks may be cold. */
static int
xg_aux_child_is_active(const unsigned char *c)
{
	u32 ctl, port;

	if (gXgAuxDisplay == NULL || DISPLAY_VER(gXgAuxDisplay) != 14)
		return 0;
	ctl = intel_de_read(gXgAuxDisplay, TRANS_DDI_FUNC_CTL(gXgAuxDisplay, TRANSCODER_A));
	if (!(ctl & TRANS_DDI_FUNC_ENABLE) ||
	    (ctl & TRANS_DDI_MODE_SELECT_MASK) != TRANS_DDI_MODE_SELECT_DP_SST)
		return 0;
	port = (ctl & TGL_TRANS_DDI_PORT_MASK) >> TGL_TRANS_DDI_PORT_SHIFT;
	return port > 0 && xg_display_dvo_port(gXgAuxDisplay, c[16]) == (int)port - 1;
}

int
xg_display_aux_probe(void)
{
	unsigned char edid[XG_DISPLAY_MAX_EDID], dpcd[XG_DISPLAY_DPCD_BYTES];
	unsigned int i, len = 0, matches = 0, attempted = 0;
	int selected = -1;

	gXgDpChildIndex = -1;
	for (i = 0; i < (unsigned int)gXgChildCount; i++) {
		const unsigned char *c = gXgChildData + i * gXgChildStride;
		if (!xg_display_external_dp()) {
			if (gXgChildSize >= 26 && (xg_kms_vbt_u16(c, 2) & (1U << 12)))
				return xg_display_aux_probe_child(c);
			continue;
		}
		if (gXgChildSize < 34 || !xg_dp_vbt_candidate(xg_kms_vbt_u16(c, 2), c[25],
		    c[33] & 1U, c[33] & 2U))
			continue;
		if (!xg_aux_child_is_active(c))
			continue;
		attempted++;
		if (xg_display_aux_probe_child(c) != 0)
			continue;
		if (matches++ == 0) {
			selected = (int)i;
			len = gXgEdidLen;
			memcpy(edid, gXgEdid, len);
			memcpy(dpcd, gXgDpcd, sizeof(dpcd));
		}
	}
	if (xg_display_external_dp() && matches == 1) {
		gXgDpChildIndex = selected;
		gXgEdidLen = len;
		gXgDpcdOk = 1;
		memcpy(gXgEdid, edid, len);
		memcpy(gXgDpcd, dpcd, sizeof(dpcd));
		snprintf(gXgAuxWhy, sizeof(gXgAuxWhy), "external DP: unique EDID at VBT child %d", selected);
		xg_disp_line(gXgAuxDisplay, gXgAuxWhy);
		return 0;
	}
	gXgEdidLen = 0;
	gXgDpcdOk = 0;
	if (!xg_display_external_dp())
		return xg_display_aux_probe_child(NULL);
	snprintf(gXgAuxWhy, sizeof(gXgAuxWhy), "external DP: %u responding outputs from %u AUX attempt(s); exactly one required", matches, attempted);
	if (gXgAuxDisplay != NULL)
		xg_disp_line(gXgAuxDisplay, gXgAuxWhy);
	return -ENODEV;
}

unsigned int
xg_display_edid_get(const void **data)
{
	*data = gXgEdid;
	return (gXgEdidLen);
}

unsigned int
xg_display_dpcd_get(const void **data)
{
	*data = gXgDpcd;
	return (gXgDpcdOk ? XG_DISPLAY_DPCD_BYTES : 0);
}

/*
 * K1 (staged eDP takeover, docs/03-PLAN §30): i915 builds the internal panel's encoder/connector and reads the
 * firmware-lit pipe back, but commits nothing -- the way NVKMS first adopts the boot head before NVRMFB commits a
 * mode of its own.  compat.linuxkpi.i915_xegfx_kms (set by the core from NVRAM xegfx-disp-kms) picks the stage:
 * 0 = display left alone (today), 1 = build outputs + read out, no sanitize, no initial commit, no fbdev,
 * 2 = K2a: K1 plus xg_display_kms_check().
 */
#include "intel_dp.h"
#include "intel_atomic.h"
#include "intel_cdclk.h"
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_uapi.h>
#include <drm/drm_probe_helper.h>
#include <drm/display/drm_dp_helper.h>

static int gXgKmsLevel = -1;

int
xg_display_kms_level(void)
{
	int level = 0;

	if (gXgKmsLevel >= 0)
		return (gXgKmsLevel);
	getenv_int("compat.linuxkpi.i915_xegfx_kms", &level);
	if (level < 0 || level > 3)
		level = 0;
	gXgKmsLevel = level;
	return (level);
}

/* what i915 now knows about the panel and the pipe the firmware left running; read-only, into debug.xegfx_disp */
void
xg_display_kms_dump(struct intel_display *display)
{
	struct drm_connector_list_iter it;
	struct drm_connector *dc;
	struct intel_crtc *crtc;
	char line[192];

	snprintf(line, sizeof(line), "kms: level=%d pipes=%d cdclk=%u max_cdclk=%u max_dotclk=%u",
	    xg_display_kms_level(), INTEL_NUM_PIPES(display), display->cdclk.hw.cdclk,
	    display->cdclk.max_cdclk_freq, display->cdclk.max_dotclk_freq);
	xg_disp_line(display, line);
	drm_connector_list_iter_begin(display->drm, &it);
	drm_for_each_connector_iter(dc, &it) {
		struct intel_connector *c = to_intel_connector(dc);
		struct intel_encoder *e = intel_attached_encoder(c);
		const struct drm_display_mode *m;
		int n = 0;

		snprintf(line, sizeof(line), "kms: connector %s type %d encoder %s", dc->name, dc->connector_type,
		    e ? e->base.name : "none");
		xg_disp_line(display, line);
		if (e && intel_encoder_is_dp(e)) {
			struct intel_dp *dp = enc_to_intel_dp(e);
			snprintf(line, sizeof(line), "kms:   dp rev 0x%02x max_rate %d kHz lanes %d sink_rates %d dsc %d (rev 0x%02x) edp %d",
			    dp->dpcd[DP_DPCD_REV], intel_dp_max_link_rate(dp), intel_dp_max_lane_count(dp), dp->num_sink_rates,
			    drm_dp_sink_supports_dsc(c->dp.dsc_dpcd) ? 1 : 0, c->dp.dsc_dpcd[DP_DSC_REV - DP_DSC_SUPPORT],
			    intel_dp_is_edp(dp) ? 1 : 0);
			xg_disp_line(display, line);
		}
		list_for_each_entry(m, &c->panel.fixed_modes, head) {
			snprintf(line, sizeof(line), "kms:   fixed[%d] %dx%d clock %d kHz htot %d vtot %d %d Hz type 0x%x",
			    n++, m->hdisplay, m->vdisplay, m->clock, m->htotal, m->vtotal, drm_mode_vrefresh(m), m->type);
			xg_disp_line(display, line);
		}
	}
	drm_connector_list_iter_end(&it);
	for_each_intel_crtc(display->drm, crtc) {
		const struct intel_crtc_state *s = to_intel_crtc_state(crtc->base.state);
		const struct drm_display_mode *a = &s->hw.adjusted_mode;

		snprintf(line, sizeof(line), "kms: crtc %s active %d %dx%d clock %d kHz bpp %d port_clock %d lanes %d dsc %d outputs 0x%x trans %d",
		    crtc->base.name, s->hw.active ? 1 : 0, a->crtc_hdisplay, a->crtc_vdisplay, a->crtc_clock, s->pipe_bpp,
		    s->port_clock, s->lane_count, s->dsc.compression_enable ? 1 : 0, s->output_types, s->cpu_transcoder);
		xg_disp_line(display, line);
	}
}

/*
 * K2a: what would i915 program?  One atomic check per case -- drm_atomic_check_only(), never a commit, so the commit
 * tail and every register write stay out of reach: first the read-out state the way intel_initial_commit() builds it
 * (affected planes, the encoders' initial_fastset_check), then each panel fixed mode on the same pipe.  The verdict
 * (fastset or full modeset) and the link, DSC and CDCLK i915 chose go to debug.xegfx_disp; with drm.debug KMS|ATOMIC
 * on for the duration, i915's own "fastset requirement not met" lines and the crtc state dump go to the kernel log.
 *
 * K2a.2 adds what K2 will commit with: a placeholder framebuffer of the adapter's scanout geometry (2560x1600
 * XRGB8888 linear, shmem, never pinned or mapped -- a check does not prepare planes) on the primary plane, so the plane,
 * DDB and watermarks are checked too; one more 240 Hz case with force_dsc_en, to see the DSC configuration i915 would
 * pick where NVKMS uses DSC (docs/31 §31.4: 2 slices of 1280, 16.06 bpp); and a dump of every input of the bpp and
 * link-rate choice (K2a picked 18 bpp at HBR3 for 60 Hz, which neither the VBT fixup nor slow-and-wide explains).
 */
#include "intel_fb.h"
#include "intel_panel.h"
#include "intel_backlight.h"
#include "intel_display_driver.h"
#include "gem/i915_gem_object.h"
#include <drm/drm_fourcc.h>
#include <drm/drm_vblank.h>
#include <linux/wait.h>

/*
 * K3 state.  Written once at the end of the K2 boot commit (probe thread), read by the KAPI afterwards; setMode
 * serializes itself with gXgKmsModeLock, i915's modeset locks do the rest.
 */
static int			 gXgKmsCommitted;
/*
 * Set last, after the fixed-mode table and the pointers below are complete.  gXgKmsCommitted turns 1 the moment the
 * boot commit returns, before xg_kms_capture_fixed_modes() runs; an adapter polling getTiming() in that gap read mode 0
 * and then missed mode 2 (2026-10-30 11:34, no 240 Hz, no scaled desktops, and a black desktop).
 */
static int			 gXgKmsModesReady;
/*
 * J1 joiner (docs/03-PLAN §30.24): the pipe is joined with pipe B when one pipe cannot carry the plane -- a 300 Hz
 * raster (over max_dotclk) or a desktop shrunk onto the raster so much that the plane's pixel rate over two pixels
 * per clock exceeds max_cdclk (3840x2400 -> 2560x1600 at 240 Hz needs ~1.25 GHz of a 652.8 MHz CDCLK).  Joined, each
 * pipe scans half.  gXgKmsJoinerDelta is pipe B's PLANE_SURF minus pipe A's as i915 programmed them for the
 * placeholder: the accel writes PLANE_SURF_1_B = frame + delta next to PLANE_SURF_1_A (xgJoinerInfo).
 */
static int			 gXgKmsJoined;
static unsigned int		 gXgKmsJoinerDelta;
static unsigned int		 gXgKmsJoinerGen;
static int			 gXgKmsLastCommitRet;
static struct intel_display	*gXgKmsDisplay;
static struct intel_crtc	*gXgKmsCrtc;
static struct intel_connector	*gXgKmsEdp;
static struct drm_framebuffer	*gXgKmsFb;	/* the placeholder; the committed plane state holds the reference */
static struct drm_i915_gem_object *gXgKmsObj;	/* backs every placeholder size; this reference is never dropped */
static const struct drm_display_mode *gXgKmsModes[XG_KMS_MAX_FIXED_MODES];
static unsigned int		 gXgKmsModeCount;
static int			 gXgKmsModeCurrent = -1;
static DEFINE_MUTEX(gXgKmsModeLock);
/* K4: the mode to boot into, from NVRAM (xg_kext.cpp, before stage 4); 0 = the panel's preferred mode */
static unsigned int		 gXgBootReqMHz, gXgBootReqW, gXgBootReqH;
static int			 gXgBootSaved;	/* the boot commit ran the saved mode */

void xg_display_set_boot_mode(unsigned int refresh_mhz, unsigned int width, unsigned int height);
void
xg_display_set_boot_mode(unsigned int refresh_mhz, unsigned int width, unsigned int height)
{
	gXgBootReqMHz = refresh_mhz;
	gXgBootReqW = width;
	gXgBootReqH = height;
}

/* External DP has EDID modes, not an eDP panel fixed-mode list. */
static struct list_head *xg_kms_mode_list(struct intel_connector *conn)
{
	return xg_display_external_dp() ? &conn->base.modes : &conn->panel.fixed_modes;
}

static int xg_kms_mode_allowed(const struct drm_display_mode *m)
{
	return !xg_display_external_dp() || (m->status == MODE_OK && m->clock > 0 &&
	    xg_dp_mode_accepted(m->hdisplay, m->vdisplay, drm_mode_vrefresh(m),
	    (m->flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLSCAN)) != 0));
}

static const struct drm_display_mode *xg_kms_preferred_mode(struct intel_connector *conn)
{
	const struct drm_display_mode *m, *best = NULL;

	if (!xg_display_external_dp())
		return intel_panel_preferred_fixed_mode(conn);
	list_for_each_entry(m, &conn->base.modes, head) {
		if (!xg_kms_mode_allowed(m))
			continue;
		if (best == NULL || m->hdisplay * m->vdisplay > best->hdisplay * best->vdisplay ||
		    (m->hdisplay * m->vdisplay == best->hdisplay * best->vdisplay &&
		    drm_mode_vrefresh(m) > drm_mode_vrefresh(best)))
			best = m;
	}
	return best;
}

/* K4: the fixed mode with the preferred raster at the saved refresh, and a desktop the plane can take; else NULL */
static const struct drm_display_mode *
xg_kms_boot_pick(struct intel_connector *edp, const struct drm_display_mode *pref, unsigned int *w, unsigned int *h)
{
	const struct drm_display_mode *m;

	*w = *h = 0;
	if (gXgBootReqMHz == 0 || pref == NULL)
		return (NULL);
	if (gXgBootReqW < 640 || gXgBootReqH < 400 || gXgBootReqW > XG_KMS_PLANE_MAX_W || gXgBootReqH > XG_KMS_PLANE_MAX_H ||
	    (gXgBootReqW & 15U) != 0 || gXgBootReqW > 2U * (unsigned int)pref->hdisplay ||
	    gXgBootReqH > 2U * (unsigned int)pref->vdisplay)
		return (NULL);
	list_for_each_entry(m, xg_kms_mode_list(edp), head) {
		if (!xg_kms_mode_allowed(m))
			continue;
		const unsigned int mhz = (unsigned int)drm_mode_vrefresh(m) * 1000U;

		if (m->hdisplay != pref->hdisplay || m->vdisplay != pref->vdisplay)
			continue;
		if (mhz + 1000U > gXgBootReqMHz && mhz < gXgBootReqMHz + 1000U) {
			*w = gXgBootReqW;
			*h = gXgBootReqH;
			return (m);
		}
	}
	return (NULL);
}

static unsigned int
xg_kms_boot_state(unsigned int *out13)
{
	if (xg_display_kms_level() < 3)
		return (XG_KMS_ERR_UNSUPPORTED);
	if (!READ_ONCE(gXgKmsModesReady))
		return (XG_KMS_ERR_NOT_READY);
	rmb();
	if (gXgKmsModeCurrent < 0 || gXgKmsFb == NULL)
		return (XG_KMS_ERR_NOT_FOUND);
	memset(out13, 0, XG_KMS_TIMING_BYTES);
	out13[0] = XG_KMS_BOOT_STATE_TAG;
	out13[1] = (unsigned int)gXgKmsModeCurrent;
	out13[2] = gXgKmsFb->width;
	out13[3] = gXgKmsFb->height;
	out13[4] = gXgKmsFb->pitches[0];
	out13[5] = (unsigned int)gXgKmsJoined;
	out13[6] = (unsigned int)gXgBootSaved;
	return (XG_KMS_OK);
}

static void xg_kms_check_one(struct intel_display *display, struct intel_crtc *crtc, struct intel_connector *conn,
    struct drm_framebuffer *fb, const struct drm_display_mode *mode, bool force_dsc, int max_bpc, bool commit,
    const char *tag);

static void
xg_kms_capture_fixed_modes(struct intel_display *display, struct intel_crtc *crtc, struct intel_connector *edp,
    struct drm_framebuffer *fb, const struct drm_display_mode *pref)
{
	const struct drm_display_mode *m;
	char line[160];

	gXgKmsDisplay = display;
	gXgKmsCrtc = crtc;
	gXgKmsEdp = edp;
	gXgKmsFb = fb;
	gXgKmsModeCount = 0;
	list_for_each_entry(m, xg_kms_mode_list(edp), head) {
		if (!xg_kms_mode_allowed(m))
			continue;
		if (gXgKmsModeCount >= XG_KMS_MAX_FIXED_MODES)
			break;
		if (m == pref)
			gXgKmsModeCurrent = (int)gXgKmsModeCount;
		snprintf(line, sizeof(line), "kms: K3 fixed mode %u: %dx%d@%d clock %d kHz%s", gXgKmsModeCount,
		    m->hdisplay, m->vdisplay, drm_mode_vrefresh(m), m->clock, m == pref ? " (committed at boot)" : "");
		xg_disp_line(display, line);
		gXgKmsModes[gXgKmsModeCount++] = m;
	}
	snprintf(line, sizeof(line), "kms: B1 backlight present %d enabled %d level %u (min %u max %u) = %u%%",
	    edp->panel.backlight.present ? 1 : 0, edp->panel.backlight.enabled ? 1 : 0, edp->panel.backlight.level,
	    edp->panel.backlight.min, edp->panel.backlight.max, xg_kms_backlight_pct(&edp->panel));
	xg_disp_line(display, line);
}

/* fixed mode n as the 13 NVK5 words getTiming() returns for the current raster */
static unsigned int
xg_kms_fixed_mode_timing(unsigned int n, unsigned int *out13)
{
	const struct drm_display_mode *m;

	/* NOT_READY means "the boot commit is still coming": the adapter waits for it; below K2 it never comes */
	if (xg_display_kms_level() < 3)
		return (XG_KMS_ERR_UNSUPPORTED);
	if (!READ_ONCE(gXgKmsModesReady))
		return (XG_KMS_ERR_NOT_READY);
	rmb();
	if (n >= gXgKmsModeCount)
		return (XG_KMS_ERR_NOT_FOUND);
	m = gXgKmsModes[n];
	memset(out13, 0, XG_KMS_TIMING_BYTES);
	out13[0] = XG_KMS_TIMING_TAG;
	out13[1] = (unsigned int)m->clock * 1000U;
	out13[2] = m->hdisplay;
	out13[3] = m->hsync_start;
	out13[4] = m->hsync_end;
	out13[5] = m->htotal;
	out13[6] = m->vdisplay;
	out13[7] = m->vsync_start;
	out13[8] = m->vsync_end;
	out13[9] = m->vtotal;
	out13[10] = (unsigned int)(((unsigned long long)m->clock * 1000000ULL) / ((unsigned long long)m->htotal * m->vtotal));
	out13[11] = (m->flags & DRM_MODE_FLAG_PHSYNC) ? 1 : 0;
	out13[12] = (m->flags & DRM_MODE_FLAG_PVSYNC) ? 1 : 0;
	return (xg_kms_timing_ok(out13) ? XG_KMS_OK : XG_KMS_ERR_NOT_FOUND);
}

static unsigned int
xg_kms_current_timing(unsigned int *out13)
{
	if (xg_display_kms_level() < 3)
		return XG_KMS_ERR_UNSUPPORTED;
	if (!READ_ONCE(gXgKmsModesReady))
		return XG_KMS_ERR_NOT_READY;
	rmb();
	if (gXgKmsModeCurrent < 0)
		return XG_KMS_ERR_NOT_FOUND;
	return xg_kms_fixed_mode_timing((unsigned int)gXgKmsModeCurrent, out13);
}

/* J1: can one pipe carry fixed mode m with a w x h desktop on its plane?  The same arithmetic i915 checks */
static int
xg_kms_needs_joiner(const struct drm_display_mode *m, unsigned int w, unsigned int h)
{
	unsigned long long rate = (unsigned long long)m->clock;

	if (xg_display_external_dp())
		return 0;

	if (m->clock > gXgKmsDisplay->cdclk.max_dotclk_freq)
		return (1);
	if (w > (unsigned int)m->hdisplay)
		rate = rate * w / m->hdisplay;		/* intel_adjusted_rate(): only downscaling raises it */
	if (h > (unsigned int)m->vdisplay)
		rate = rate * h / m->vdisplay;
	return (rate / 2 > (unsigned long long)gXgKmsDisplay->cdclk.max_cdclk_freq);
}

/* J1: after a commit, whether pipe A is joined and where i915 pointed pipe B's plane relative to pipe A's */
static void
xg_kms_update_joiner(void)
{
	const struct intel_crtc_state *cs = to_intel_crtc_state(gXgKmsCrtc->base.state);
	unsigned int a = intel_de_read(gXgKmsDisplay, _MMIO(0x7019c)) & ~0xfffU;
	unsigned int b = intel_de_read(gXgKmsDisplay, _MMIO(0x7119c)) & ~0xfffU;
	char line[160];

	gXgKmsJoined = cs->joiner_pipes != 0;
	gXgKmsJoinerDelta = gXgKmsJoined ? b - a : 0;
	gXgKmsJoinerGen++;
	snprintf(line, sizeof(line), "kms: J1 joiner %s (pipes 0x%x) PLANE_SURF A 0x%x B 0x%x delta 0x%x gen %u",
	    gXgKmsJoined ? "ON" : "off", cs->joiner_pipes, a, b, gXgKmsJoinerDelta, gXgKmsJoinerGen);
	xg_disp_line(gXgKmsDisplay, line);
	xg_kms_cursor_reapply();	/* C1: every commit puts i915's own cursor state back on the registers */
}

/* J1: what the accel needs to keep a joined pipe B on the frame it flips to (xg_kext.cpp "xgJoinerInfo") */
int
xg_display_kms_joiner_info(unsigned int *active, unsigned int *delta, unsigned int *gen)
{
	if (active != NULL)
		*active = (unsigned int)gXgKmsJoined;
	if (delta != NULL)
		*delta = gXgKmsJoinerDelta;
	if (gen != NULL)
		*gen = gXgKmsJoinerGen;
	return (gXgKmsCommitted ? 0 : -EAGAIN);
}

/*
 * §37.28 hardware colour.  Since the AGDC answers the display pipe's colour stages (0030), CoreDisplay no longer runs its
 * full-screen GPUPass: it hands the linearization, the colour matrix and the encoding to the display pipe in each
 * transaction, as Apple's ICL driver expects (IGAccelDisplayPipe::PerformGammaTransaction / PerformCscTransaction).
 * Until now nothing applied them, so Night Shift and colour profiles had no effect.  Here they become i915's crtc
 * DEGAMMA_LUT, CTM and GAMMA_LUT, committed with the crtc only (i915 copies them to a joined pipe B).  The tables are
 * resampled to the pipe's LUT sizes.  LUT loads go through MMIO, not the DSB, which nothing else here uses yet.
 */
static u16
xg_colour_f32_q16(u32 bits)
{
	const u32 e = (bits >> 23) & 0xff;
	u64 m, v;
	int sh;

	if ((bits & 0x80000000u) || e == 0)
		return (0);
	if (e == 0xff)
		return (0xffff);
	m = (u64)((bits & 0x7fffffu) | 0x800000u) * 65535u;
	sh = 150 - (int)e;
	if (sh <= 0)
		return (0xffff);
	if (sh >= 63)
		return (0);
	v = (m + (1ull << (sh - 1))) >> sh;
	return (v > 0xffff ? 0xffff : (u16)v);
}

/* S31.32 sign-magnitude, as struct drm_color_ctm wants it */
static u64
xg_colour_f32_s3132(u32 bits)
{
	const u32 e = (bits >> 23) & 0xff;
	const u64 sign = (u64)(bits >> 31) << 63;
	u64 m, mag;
	int sh;

	if (e == 0)
		return (0);
	m = (u64)((bits & 0x7fffffu) | 0x800000u);
	sh = (int)e - 118;	/* value * 2^32 = m * 2^(e - 150 + 32) */
	if (e == 0xff || sh >= 40)
		mag = 0x7fffffffffffffffull;
	else if (sh >= 0)
		mag = m << sh;
	else if (sh > -63)
		mag = m >> -sh;
	else
		mag = 0;
	return (sign | (mag & 0x7fffffffffffffffull));
}

/* n RGB entries (float bits) resampled to size entries */
static struct drm_property_blob *
xg_colour_lut_blob(struct drm_device *dev, const u32 *rgb, unsigned int n, unsigned int size)
{
	struct drm_property_blob *blob;
	struct drm_color_lut *lut;
	unsigned int i, ch;

	if (n < 2 || size < 2)
		return (NULL);
	blob = drm_property_create_blob(dev, sizeof(*lut) * size, NULL);
	if (IS_ERR(blob))
		return (blob);
	lut = blob->data;
	for (i = 0; i < size; i++) {
		const u64 pos = (u64)i * (n - 1) * 65536u / (size - 1);
		const unsigned int j = (unsigned int)(pos >> 16) < n - 1 ? (unsigned int)(pos >> 16) : n - 2;
		const s64 f = (s64)(pos - ((u64)j << 16));
		u16 out[3];

		for (ch = 0; ch < 3; ch++) {
			const s64 a = xg_colour_f32_q16(rgb[3 * j + ch]), b = xg_colour_f32_q16(rgb[3 * (j + 1) + ch]);
			s64 v = a + (((b - a) * f) >> 16);

			out[ch] = (u16)(v < 0 ? 0 : v > 0xffff ? 0xffff : v);
		}
		lut[i].red = out[0];
		lut[i].green = out[1];
		lut[i].blue = out[2];
		lut[i].reserved = 0;
	}
	return (blob);
}

static struct drm_property_blob *
xg_colour_ctm_blob(struct drm_device *dev, const struct xg_pipe_colour *c, int *offsets)
{
	struct drm_property_blob *blob;
	struct drm_color_ctm *ctm;
	unsigned int r, k;

	blob = drm_property_create_blob(dev, sizeof(*ctm), NULL);
	if (IS_ERR(blob))
		return (blob);
	ctm = blob->data;
	*offsets = 0;
	for (r = 0; r < 3; r++) {
		for (k = 0; k < 3; k++) {
			const unsigned int rr = (c->matrix_mode & 2) ? k : r, cc = (c->matrix_mode & 2) ? r : k;
			const u32 bits = (c->matrix_mode & 1) ? c->matrix[rr * 3 + cc] : c->matrix[rr * 4 + cc];

			ctm->matrix[r * 3 + k] = xg_colour_f32_s3132(bits);
		}
		if (((c->matrix_mode & 1) ? c->matrix[9 + r] : c->matrix[r * 4 + 3]) & 0x7fffffffu)
			*offsets = 1;
	}
	return (blob);
}

int
xg_display_kms_colour(const struct xg_pipe_colour *c, unsigned int *us_out)
{
	struct intel_display *display = gXgKmsDisplay;
	struct intel_crtc *crtc = gXgKmsCrtc;
	struct drm_property_blob *pre = NULL, *ctm = NULL, *post = NULL;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_atomic_state *state;
	struct drm_crtc_state *cs;
	unsigned int dsz, gsz;
	char line[200];
	int ret, offsets = 0;
	u64 t0;

	if (c == NULL || c->version != XG_PIPE_COLOUR_VERSION || c->npre > XG_PIPE_COLOUR_MAX ||
	    c->npost > XG_PIPE_COLOUR_MAX)
		return (-EINVAL);
	if (!gXgKmsCommitted || display == NULL || crtc == NULL)
		return (-EAGAIN);
	dsz = DISPLAY_INFO(display)->color.degamma_lut_size;
	gsz = DISPLAY_INFO(display)->color.gamma_lut_size;
	if ((c->flags & XG_PIPE_COLOUR_PRE) && dsz)
		pre = xg_colour_lut_blob(display->drm, c->pre, c->npre, dsz);
	if ((c->flags & XG_PIPE_COLOUR_MATRIX))
		ctm = xg_colour_ctm_blob(display->drm, c, &offsets);
	if ((c->flags & XG_PIPE_COLOUR_POST) && gsz)
		post = xg_colour_lut_blob(display->drm, c->post, c->npost, gsz);
	if (IS_ERR(pre) || IS_ERR(ctm) || IS_ERR(post)) {
		ret = -ENOMEM;
		goto blobs;
	}
	t0 = ktime_get_ns();
	mutex_lock(&gXgKmsModeLock);
	display->params.enable_dsb = false;
	state = drm_atomic_state_alloc(display->drm);
	if (state == NULL) {
		mutex_unlock(&gXgKmsModeLock);
		ret = -ENOMEM;
		goto blobs;
	}
	drm_modeset_acquire_init(&ctx, 0);
	state->acquire_ctx = &ctx;
	to_intel_atomic_state(state)->internal = true;
retry:
	cs = drm_atomic_get_crtc_state(state, &crtc->base);
	if (IS_ERR(cs)) {
		ret = PTR_ERR(cs);
		goto out;
	}
	cs->color_mgmt_changed |= drm_property_replace_blob(&cs->degamma_lut, pre);
	cs->color_mgmt_changed |= drm_property_replace_blob(&cs->ctm, ctm);
	cs->color_mgmt_changed |= drm_property_replace_blob(&cs->gamma_lut, post);
	ret = cs->color_mgmt_changed ? drm_atomic_commit(state) : 0;
out:
	if (ret == -EDEADLK) {
		drm_atomic_state_clear(state);
		drm_modeset_backoff(&ctx);
		goto retry;
	}
	drm_atomic_state_put(state);
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
	if (ret == 0)
		xg_kms_update_joiner();
	mutex_unlock(&gXgKmsModeLock);
	if (us_out != NULL)
		*us_out = (unsigned int)((ktime_get_ns() - t0) / 1000);
	snprintf(line, sizeof(line), "kms: colour degamma %u->%u ctm %s%s gamma %u->%u: %d in %llu us",
	    pre ? c->npre : 0, pre ? dsz : 0, ctm ? "set" : "off", offsets ? " (offsets ignored)" : "",
	    post ? c->npost : 0, post ? gsz : 0, ret, (unsigned long long)((ktime_get_ns() - t0) / 1000));
	xg_disp_line(display, line);
blobs:
	if (!IS_ERR_OR_NULL(pre))
		drm_property_blob_put(pre);
	if (!IS_ERR_OR_NULL(ctm))
		drm_property_blob_put(ctm);
	if (!IS_ERR_OR_NULL(post))
		drm_property_blob_put(post);
	return (ret);
}

/*
 * K3: commit fixed mode n from the adapter's setDisplayMode.  Same rules as the K2 boot commit (DRRS off, 8 bpc, PSR
 * off, the placeholder stays on the primary plane), plus DSC forced above 60 Hz: on its own i915 drops 240 Hz to
 * 6 bpc rather than compress (§30.9).  A mode that needs the joiner is refused: the accel writes PLANE_SURF_1_A only,
 * so pipe B's half of a joined 300 Hz frame would stay on the placeholder.  Blocking, about 1 s (a full modeset, as
 * NVKMS does for every refresh change); the caller is between IOFramebuffer's mode-change notifications, so the accel
 * does not flip meanwhile.
 */
static unsigned int
xg_kms_set_mode(void *ctx, unsigned int fb_index, unsigned int mode_id)
{
	const struct drm_display_mode *m;
	struct intel_crtc_state *cs;
	char tag[48], line[160];
	int hz;
	unsigned int rc;

	(void)ctx;
	if (!gXgKmsCommitted)
		return (XG_KMS_ERR_NOT_READY);
	if (fb_index != 0)
		return (XG_KMS_ERR_NOT_FOUND);
	if (mode_id >= gXgKmsModeCount)
		return (XG_KMS_ERR_BAD_ARGUMENT);
	m = gXgKmsModes[mode_id];
	hz = drm_mode_vrefresh(m);
	(void)line;
	mutex_lock(&gXgKmsModeLock);
	if ((int)mode_id == gXgKmsModeCurrent) {
		mutex_unlock(&gXgKmsModeLock);
		return (XG_KMS_OK);
	}
	gXgKmsCommitted = 2;	/* set_mode in flight; the commit below sets it back to 1 */
	/* J1: join when this raster or the desktop on the plane is too much for one pipe (300 Hz, HiDPI at 240) */
	gXgKmsEdp->force_joined_pipes = xg_kms_needs_joiner(m, gXgKmsFb->width, gXgKmsFb->height) ? 2 : 0;
	snprintf(tag, sizeof(tag), "K3 %dx%d@%d/8bpc%s%s", m->hdisplay, m->vdisplay, hz, hz > 60 ? "+dsc" : "",
	    gXgKmsEdp->force_joined_pipes ? "+joiner" : "");
	xg_kms_check_one(gXgKmsDisplay, gXgKmsCrtc, gXgKmsEdp, gXgKmsFb, m, !xg_display_external_dp() && hz > 60, 8, true, tag);
	xg_kms_update_joiner();
	cs = to_intel_crtc_state(gXgKmsCrtc->base.state);
	if (gXgKmsCommitted == 1 && cs->hw.adjusted_mode.crtc_clock == m->clock) {
		gXgKmsModeCurrent = (int)mode_id;
		rc = XG_KMS_OK;
	} else {
		gXgKmsCommitted = 1;
		rc = XG_KMS_ERR_BUSY;
	}
	mutex_unlock(&gXgKmsModeLock);
	return (rc);
}

/* B1: the panel's backlight, as a percent of its own min..max */
static unsigned int
xg_kms_backlight_pct(const struct intel_panel *panel)
{
	u32 min = panel->backlight.min, max = panel->backlight.max, level = panel->backlight.level;

	if (max <= min)
		return (0);
	if (level <= min)
		return (0);
	return ((unsigned int)(((u64)(level - min) * 100 + (max - min) / 2) / (max - min)));
}

static unsigned int
xg_kms_backlight(void *ctx, int set_percent, unsigned int *percent_out)
{
	struct intel_connector *conn = gXgKmsEdp;
	struct drm_device *dev;
	struct intel_panel *panel;
	char line[160];

	(void)ctx;
	if (!READ_ONCE(gXgKmsModesReady) || conn == NULL)
		return (XG_KMS_ERR_NOT_READY);
	panel = &conn->panel;
	if (!panel->backlight.present)
		return (XG_KMS_ERR_UNSUPPORTED);
	dev = conn->base.dev;
	drm_modeset_lock(&dev->mode_config.connection_mutex, NULL);
	if (set_percent >= 0) {
		if (set_percent > 100)
			set_percent = 100;
		intel_backlight_set_acpi(conn->base.state, (u32)set_percent, 100);
	}
	if (percent_out != NULL)
		*percent_out = xg_kms_backlight_pct(panel);
	if (set_percent >= 0) {
		snprintf(line, sizeof(line), "kms: B1 backlight %d%% -> level %u (min %u max %u, enabled %d)", set_percent,
		    panel->backlight.level, panel->backlight.min, panel->backlight.max, panel->backlight.enabled ? 1 : 0);
		pr_info("XEGFX_DISP: %s\n", line);
	}
	drm_modeset_unlock(&dev->mode_config.connection_mutex);
	return (XG_KMS_OK);
}

/*
 * E3: the adapter's vblank thread sleeps here instead of on a free-running timer.  A vblank reference per call keeps
 * the pipe's vblank interrupt on while the adapter keeps asking (i915 disables it at the first vblank with no
 * reference, vblank_disable_immediate); drm_handle_vblank() wakes the queue at every vblank, and drm_crtc_vblank_off()
 * at a modeset wakes it too and fails the next get until the pipe is back.
 */
static unsigned int
xg_kms_wait_vblank(void *ctx, unsigned long long last, unsigned int timeout_us, unsigned long long *count_out,
    unsigned long long *age_ns_out)
{
	struct drm_crtc *crtc;
	struct drm_vblank_crtc *vb;
	ktime_t t;
	s64 age;
	u64 c;

	(void)ctx;
	if (count_out == NULL || age_ns_out == NULL || timeout_us == 0 || timeout_us > 1000000U)
		return (XG_KMS_ERR_BAD_ARGUMENT);
	if (!READ_ONCE(gXgKmsModesReady) || gXgKmsCrtc == NULL)
		return (XG_KMS_ERR_NOT_READY);
	crtc = &gXgKmsCrtc->base;
	if (drm_crtc_vblank_get(crtc) != 0)
		return (XG_KMS_ERR_BUSY);
	vb = drm_crtc_vblank_crtc(crtc);
	(void)wait_event_timeout(vb->queue, drm_crtc_vblank_count(crtc) != last || !READ_ONCE(vb->enabled),
	    usecs_to_jiffies(timeout_us));
	c = drm_crtc_vblank_count_and_time(crtc, &t);
	drm_crtc_vblank_put(crtc);
	age = ktime_to_ns(ktime_sub(ktime_get(), t));
	*count_out = c;
	*age_ns_out = age > 0 ? (unsigned long long)age : 0;
	return (c != last ? XG_KMS_OK : XG_KMS_ERR_BUSY);
}

/* one plane-only commit: fb on the primary plane, its whole size as the source, the current raster as the target */
static int
xg_kms_plane_commit(struct drm_framebuffer *fb)
{
	struct intel_display *display = gXgKmsDisplay;
	struct intel_crtc *crtc = gXgKmsCrtc;
	struct drm_plane *primary = crtc->base.primary;
	const struct drm_display_mode *mode = &crtc->base.state->mode;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_atomic_state *state;
	struct drm_plane_state *ps;
	int ret;

	state = drm_atomic_state_alloc(display->drm);
	if (state == NULL)
		return (-ENOMEM);
	drm_modeset_acquire_init(&ctx, 0);
	state->acquire_ctx = &ctx;
	to_intel_atomic_state(state)->internal = true;
retry:
	ps = drm_atomic_get_plane_state(state, primary);
	if (IS_ERR(ps)) {
		ret = PTR_ERR(ps);
		goto out;
	}
	ret = drm_atomic_set_crtc_for_plane(ps, &crtc->base);
	if (ret)
		goto out;
	drm_atomic_set_fb_for_plane(ps, fb);
	ps->src_x = ps->src_y = 0;
	ps->src_w = (u32)fb->width << 16;
	ps->src_h = (u32)fb->height << 16;
	ps->crtc_x = ps->crtc_y = 0;
	ps->crtc_w = mode->hdisplay;
	ps->crtc_h = mode->vdisplay;
	ret = drm_atomic_commit(state);
out:
	if (ret == -EDEADLK) {
		drm_atomic_state_clear(state);
		drm_modeset_backoff(&ctx);
		goto retry;
	}
	drm_atomic_state_put(state);
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
	return (ret);
}

/*
 * A1: a scaled desktop.  The adapter's scanout and WindowServer's frames are width x height at `pitch`; the plane
 * reads them at that size (the accel still writes only PLANE_SURF, and refuses a frame whose pitch is not the plane's
 * stride) and the plane scaler stretches or shrinks them onto the raster.  No modeset: the pipe and the link stay.
 */
static unsigned int
xg_kms_set_plane(void *ctx, unsigned int width, unsigned int height, unsigned int pitch)
{
	struct drm_mode_fb_cmd2 cmd = {};
	struct drm_framebuffer *fb;
	char line[160];
	u64 t0;
	int ret;

	(void)ctx;
	if (!gXgKmsCommitted || gXgKmsObj == NULL || gXgKmsFb == NULL)
		return (XG_KMS_ERR_NOT_READY);
	if (width < 320 || height < 200 || width > XG_KMS_PLANE_MAX_W || height > XG_KMS_PLANE_MAX_H ||
	    pitch < width * 4U || (pitch & 63U) != 0 || (unsigned long long)pitch * height >
	    (unsigned long long)XG_KMS_PLANE_MAX_W * 4U * XG_KMS_PLANE_MAX_H)
		return (XG_KMS_ERR_BAD_ARGUMENT);
	mutex_lock(&gXgKmsModeLock);
	if (gXgKmsFb->width == width && gXgKmsFb->height == height && gXgKmsFb->pitches[0] == pitch &&
	    gXgKmsFb->modifier == DRM_FORMAT_MOD_LINEAR) {
		mutex_unlock(&gXgKmsModeLock);
		return (XG_KMS_OK);
	}
	cmd.width = width;
	cmd.height = height;
	cmd.pixel_format = DRM_FORMAT_XRGB8888;
	cmd.pitches[0] = pitch;
	fb = intel_framebuffer_create(intel_bo_to_drm_bo(gXgKmsObj), &cmd);
	if (IS_ERR(fb)) {
		mutex_unlock(&gXgKmsModeLock);
		snprintf(line, sizeof(line), "kms: A1 setPlane %ux%u pitch %u: framebuffer create failed (%ld)", width, height,
		    pitch, PTR_ERR(fb));
		xg_disp_line(gXgKmsDisplay, line);
		return (XG_KMS_ERR_BAD_ARGUMENT);
	}
	t0 = ktime_get_ns();
	{
		/*
		 * J1: a desktop one pipe cannot carry needs the joiner, and taking the joiner on or off is a modeset --
		 * the same fixed mode again, with the new placeholder on the plane.  Otherwise a plane-only commit.
		 */
		const struct drm_display_mode *m = gXgKmsModeCurrent >= 0 ? gXgKmsModes[gXgKmsModeCurrent] : NULL;
		int join = m != NULL && xg_kms_needs_joiner(m, width, height);

		if (m != NULL && join != gXgKmsJoined) {
			char tag[48];
			int hz = drm_mode_vrefresh(m);

			gXgKmsEdp->force_joined_pipes = join ? 2 : 0;
			snprintf(tag, sizeof(tag), "J1 %dx%d@%d desktop %ux%u%s", m->hdisplay, m->vdisplay, hz, width, height,
			    join ? "+joiner" : "");
			gXgKmsLastCommitRet = -EINVAL;
			xg_kms_check_one(gXgKmsDisplay, gXgKmsCrtc, gXgKmsEdp, fb, m, !xg_display_external_dp() && hz > 60, 8, true, tag);
			ret = gXgKmsLastCommitRet;
		} else
			ret = xg_kms_plane_commit(fb);
		if (ret == 0)
			xg_kms_update_joiner();
	}
	snprintf(line, sizeof(line), "kms: A1 setPlane %ux%u pitch %u -> raster %dx%d: %d in %llu us", width, height, pitch,
	    gXgKmsCrtc->base.state->mode.hdisplay, gXgKmsCrtc->base.state->mode.vdisplay, ret,
	    (unsigned long long)((ktime_get_ns() - t0) / 1000));
	xg_disp_line(gXgKmsDisplay, line);
	drm_framebuffer_put(fb);	/* on success the committed plane state holds its own reference */
	if (ret == 0)
		gXgKmsFb = fb;
	mutex_unlock(&gXgKmsModeLock);
	return (ret == 0 ? XG_KMS_OK : XG_KMS_ERR_UNSUPPORTED);
}

/*
 * §32.16 Tile4 scanout: the plane's layout, Tile4 (1) or linear (0).  WindowServer's frames are drawn Tile4 into their
 * own pages (a linear render target costs 8-13x the GPU time) and the accel flips them as before, writing only
 * PLANE_SURF; i915 must then scan the plane as Tile4.  This re-commits the placeholder at the same size and pitch with
 * the other modifier -- plane-only, no modeset, the joiner state stays -- so i915 programs PLANE_CTL's tiling, the
 * stride in tile units and pipe B's offset, and the joiner delta is re-read.  With DPT off (enable_dpt=0, set at boot)
 * a Tile4 plane reads the GGTT directly, as the accel's slots need.  want < 0 only reports; *cur gets the layout.
 * Called by the accel's flip worker (thread context, sleeps) when a frame's layout differs from the plane's.
 */
/*
 * §37.14 (docs/37-RENDER-COMPRESSION.md): a third layout, 2 = Tile4 with render compression
 * (I915_FORMAT_MOD_4_TILED_MTL_RC_CCS: plane 0 the Tile4 main surface, plane 1 the packed CCS at ccs_off with
 * ccs_pitch bytes per row -- main pitch / 8, one 64-byte block per four tiles across and 32 rows down).  The accel's
 * flips then point PLANE_SURF at the plugin's compressed images, which have the CCS at the same offset, so the
 * PLANE_AUX_DIST i915 programs here holds for every one of them.  The boot placeholder has no room for a CCS, so this
 * layout commits its own, made on first use (zero CCS = uncompressed) and grown if a larger one is asked for.
 */
static struct drm_i915_gem_object *gXgKmsCcsObj;	/* our reference; the committed fb holds its own */
static u64 gXgKmsCcsObjBytes;

static int
xg_kms_layout_of(const struct drm_framebuffer *fb)
{
	if (fb->modifier == I915_FORMAT_MOD_4_TILED_MTL_RC_CCS)
		return (2);
	return (fb->modifier == I915_FORMAT_MOD_4_TILED ? 1 : 0);
}

int xg_display_kms_plane_layout2(int want, int *cur, unsigned int ccs_off, unsigned int ccs_pitch);
int
xg_display_kms_plane_layout2(int want, int *cur, unsigned int ccs_off, unsigned int ccs_pitch)
{
	struct drm_mode_fb_cmd2 cmd = {};
	struct drm_framebuffer *fb, *old;
	struct drm_i915_gem_object *obj;
	char line[200];
	u64 t0, mod;
	int ret;

	if (!gXgKmsCommitted || gXgKmsObj == NULL || gXgKmsFb == NULL)
		return (-EAGAIN);
	mutex_lock(&gXgKmsModeLock);
	old = gXgKmsFb;
	if (want < 0 || (want == xg_kms_layout_of(old) &&
	    (want != 2 || (old->offsets[1] == ccs_off && old->pitches[1] == ccs_pitch)))) {
		if (cur != NULL)
			*cur = xg_kms_layout_of(old);
		mutex_unlock(&gXgKmsModeLock);
		return (0);
	}
	mod = want == 2 ? I915_FORMAT_MOD_4_TILED_MTL_RC_CCS : want ? I915_FORMAT_MOD_4_TILED : DRM_FORMAT_MOD_LINEAR;
	if (want && (old->pitches[0] % 128U) != 0) {	/* Tile4 rows are 128 bytes wide */
		mutex_unlock(&gXgKmsModeLock);
		return (-EINVAL);
	}
	cmd.width = old->width;
	cmd.height = old->height;
	cmd.pixel_format = DRM_FORMAT_XRGB8888;
	cmd.pitches[0] = old->pitches[0];
	cmd.flags = DRM_MODE_FB_MODIFIERS;
	cmd.modifier[0] = mod;
	obj = gXgKmsObj;
	if (want == 2) {
		const u64 need = PAGE_ALIGN((u64)ccs_off + (u64)ccs_pitch * DIV_ROUND_UP(cmd.height, 32U));

		if (ccs_pitch != DIV_ROUND_UP(cmd.pitches[0], 4U * 128U) * 64U || (ccs_off % 4096U) != 0 ||
		    ccs_off < (u64)cmd.pitches[0] * cmd.height) {
			mutex_unlock(&gXgKmsModeLock);
			snprintf(line, sizeof(line), "kms: RC CCS layout refused: ccs at %u pitch %u for %ux%u pitch %u",
			    ccs_off, ccs_pitch, cmd.width, cmd.height, cmd.pitches[0]);
			xg_disp_line(gXgKmsDisplay, line);
			return (-EINVAL);
		}
		if (gXgKmsCcsObj == NULL || gXgKmsCcsObjBytes < need) {
			struct drm_i915_gem_object *n = i915_gem_object_create_shmem(to_i915(gXgKmsFb->dev), need);

			if (IS_ERR(n)) {
				mutex_unlock(&gXgKmsModeLock);
				xg_disp_line(gXgKmsDisplay, "kms: RC CCS layout: placeholder object failed");
				return ((int)PTR_ERR(n));
			}
			if (gXgKmsCcsObj != NULL)
				i915_gem_object_put(gXgKmsCcsObj);	/* a committed fb over it keeps its own reference */
			gXgKmsCcsObj = n;
			gXgKmsCcsObjBytes = need;
		}
		obj = gXgKmsCcsObj;
		cmd.offsets[1] = ccs_off;
		cmd.pitches[1] = ccs_pitch;
		cmd.modifier[1] = mod;
		cmd.handles[1] = cmd.handles[0];
	}
	fb = intel_framebuffer_create(intel_bo_to_drm_bo(obj), &cmd);
	if (IS_ERR(fb)) {
		mutex_unlock(&gXgKmsModeLock);
		snprintf(line, sizeof(line), "kms: layout %d: framebuffer create failed (%ld)", want, PTR_ERR(fb));
		xg_disp_line(gXgKmsDisplay, line);
		return ((int)PTR_ERR(fb));
	}
	t0 = ktime_get_ns();
	ret = xg_kms_plane_commit(fb);
	if (ret == 0)
		xg_kms_update_joiner();
	snprintf(line, sizeof(line), "kms: layout %s %ux%u pitch %u%s: %d in %llu us",
	    want == 2 ? "Tile4 RC CCS" : want ? "Tile4" : "linear", cmd.width, cmd.height, cmd.pitches[0],
	    want == 2 ? " (ccs offset/pitch set)" : "", ret, (unsigned long long)((ktime_get_ns() - t0) / 1000));
	xg_disp_line(gXgKmsDisplay, line);
	drm_framebuffer_put(fb);	/* on success the committed plane state holds its own reference */
	if (ret == 0)
		gXgKmsFb = fb;
	if (cur != NULL)
		*cur = xg_kms_layout_of(gXgKmsFb);
	mutex_unlock(&gXgKmsModeLock);
	return (ret);
}

/* §32.16: the two-layout entry point the accel has always called (1 = Tile4, 0 = linear) */
int xg_display_kms_plane_layout(int want, int *cur);
int
xg_display_kms_plane_layout(int want, int *cur)
{
	return (xg_display_kms_plane_layout2(want < 0 ? -1 : want != 0, cur, 0, 0));
}

/*
 * C1 hardware cursor (native-display docs/32-FRAME-RATE.md §32.18).  Without one WindowServer draws the pointer into
 * every 3840x2400 composite, so it moves only as often as a whole frame completes.  The cursor plane is i915's, but
 * i915 moves it with a full atomic commit while the pipe is joined (intel_legacy_cursor_update has no joiner fast
 * path), far too slow per mouse event.  So i915 enables the plane once -- a 128x128 ARGB framebuffer straddling the
 * joiner seam, which makes it allocate DDB and watermarks for the cursor on both pipes -- and from then on the cursor
 * is ours on the registers, as PLANE_SURF is the accel's: CUR_BASE points at our own contiguous WC buffer (GGTT-mapped
 * like a KAPI scanout buffer), CUR_POS/CUR_CTL are written per pipe, CUR_BASE last (the arming write).  Every commit
 * re-applies them (xg_kms_update_joiner).  The plane is not scaled, so the image is scaled here to the raster, and
 * positions come in desktop pixels (what IOFramebuffer hands the adapter, hot spot already taken off) and are scaled
 * the same way.  KAPI reserved[6] cursorImage(ctx, argb, w, h, pitch bytes): straight-alpha ARGB, w = 0 hides it;
 * reserved[7] cursorMove(ctx, x, y, visible).  Thread context; cursorMove only writes registers.
 */
#define XG_CUR_DIM	128U
#define XG_CUR_BYTES	(XG_CUR_DIM * XG_CUR_DIM * 4U)
static DEFINE_SPINLOCK(gXgCurLock);
static xg_vm_xmem_t		 gXgCurMem;
static unsigned int		*gXgCurKva;
static unsigned long long	 gXgCurGgtt;
static struct drm_framebuffer	*gXgCurFb;	/* i915's placeholder: the committed cursor plane state holds it */
static int			 gXgCurOn;	/* the plane is enabled by i915 and our buffer is mapped */
static unsigned int		 gXgCurCtl[2];	/* CUR_CTL as i915 programmed it for pipes A and B */
static int			 gXgCurX, gXgCurY, gXgCurVis, gXgCurHave;	/* raster pixels */
static unsigned long long	 gXgCurMoves, gXgCurImages;

#define XG_CUR_CTL(p)	_MMIO(0x70080 + 0x1000 * (p))
#define XG_CUR_BASE(p)	_MMIO(0x70084 + 0x1000 * (p))
#define XG_CUR_POS(p)	_MMIO(0x70088 + 0x1000 * (p))

/* the cursor registers from our state, both halves when joined; under gXgCurLock */
static void
xg_kms_cursor_write_locked(void)
{
	const struct drm_display_mode *mode;
	int pipes, pw, ph, p;

	if (!gXgCurOn || gXgKmsCrtc == NULL)
		return;
	mode = &gXgKmsCrtc->base.state->mode;
	pipes = gXgKmsJoined ? 2 : 1;
	pw = mode->hdisplay / pipes;
	ph = mode->vdisplay;
	for (p = 0; p < pipes; p++) {
		const int x = gXgCurX - p * pw, y = gXgCurY;
		const int on = gXgCurVis && gXgCurHave && x < pw && x + (int)XG_CUR_DIM > 0 && y < ph &&
		    y + (int)XG_CUR_DIM > 0;
		const unsigned int pos = (y < 0 ? (1U << 31) | ((unsigned int)-y << 16) : (unsigned int)y << 16) |
		    (x < 0 ? (1U << 15) | (unsigned int)-x : (unsigned int)x);

		intel_de_write_fw(gXgKmsDisplay, XG_CUR_CTL(p), on ? gXgCurCtl[p] : 0);
		intel_de_write_fw(gXgKmsDisplay, XG_CUR_POS(p), pos);
		intel_de_write_fw(gXgKmsDisplay, XG_CUR_BASE(p), (unsigned int)gXgCurGgtt);
	}
}

static void
xg_kms_cursor_reapply(void)
{
	unsigned long fl;
	int p;

	spin_lock_irqsave(&gXgCurLock, fl);
	/* right after a commit the registers hold i915's state: a pipe that joined since has its CUR_CTL now */
	for (p = 0; gXgCurOn && p < (gXgKmsJoined ? 2 : 1); p++) {
		const unsigned int c = intel_de_read(gXgKmsDisplay, XG_CUR_CTL(p));

		if (c != 0)
			gXgCurCtl[p] = c;
	}
	xg_kms_cursor_write_locked();
	spin_unlock_irqrestore(&gXgCurLock, fl);
}

/* commit i915's cursor plane once with a placeholder straddling the seam, then take the registers over */
static int
xg_kms_cursor_enable(void)
{
	struct intel_display *display = gXgKmsDisplay;
	struct intel_crtc *crtc = gXgKmsCrtc;
	struct drm_i915_private *i915 = to_i915(display->drm);
	struct drm_mode_fb_cmd2 cmd = {};
	struct drm_i915_gem_object *obj;
	struct drm_framebuffer *fb;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_atomic_state *state;
	struct drm_plane_state *ps;
	const struct drm_display_mode *mode;
	unsigned long long pa = 0;
	void *kva = NULL;
	char line[160];
	int ret;

	if (gXgCurMem == NULL) {
		gXgCurMem = xg_vm_x_alloc(XG_CUR_BYTES, 0x00000000fffff000ULL, XG_VM_CACHE_WC, &kva, &pa);
		if (gXgCurMem == NULL || kva == NULL || pa == 0 || pa + XG_CUR_BYTES > 0x100000000ULL ||
		    xg_scanout_ggtt_map(XG_CUR_BYTES, pa, &gXgCurGgtt) != 0) {
			if (gXgCurMem != NULL)
				xg_vm_x_release(gXgCurMem);
			gXgCurMem = NULL;
			xg_disp_line(display, "kms: C1 cursor: no contiguous buffer below 4 GiB, or no GGTT range");
			return (-ENOMEM);
		}
		gXgCurKva = kva;
		memset(gXgCurKva, 0, XG_CUR_BYTES);
		xg_vm_x_flush(gXgCurKva, XG_CUR_BYTES);
	}
	obj = i915_gem_object_create_shmem(i915, XG_CUR_BYTES);
	if (IS_ERR(obj))
		return ((int)PTR_ERR(obj));
	cmd.width = cmd.height = XG_CUR_DIM;
	cmd.pixel_format = DRM_FORMAT_ARGB8888;
	cmd.pitches[0] = XG_CUR_DIM * 4U;
	fb = intel_framebuffer_create(intel_bo_to_drm_bo(obj), &cmd);
	i915_gem_object_put(obj);	/* the framebuffer holds its own reference */
	if (IS_ERR(fb))
		return ((int)PTR_ERR(fb));
	mode = &crtc->base.state->mode;
	state = drm_atomic_state_alloc(display->drm);
	if (state == NULL) {
		drm_framebuffer_put(fb);
		return (-ENOMEM);
	}
	drm_modeset_acquire_init(&ctx, 0);
	state->acquire_ctx = &ctx;
	to_intel_atomic_state(state)->internal = true;
retry:
	ps = drm_atomic_get_plane_state(state, crtc->base.cursor);
	if (IS_ERR(ps)) {
		ret = PTR_ERR(ps);
		goto out;
	}
	ret = drm_atomic_set_crtc_for_plane(ps, &crtc->base);
	if (ret)
		goto out;
	drm_atomic_set_fb_for_plane(ps, fb);
	ps->src_x = ps->src_y = 0;
	ps->src_w = ps->src_h = XG_CUR_DIM << 16;
	ps->crtc_x = mode->hdisplay / 2 - (int)XG_CUR_DIM / 2;	/* across the seam: both joined pipes get it */
	ps->crtc_y = 0;
	ps->crtc_w = ps->crtc_h = XG_CUR_DIM;
	ret = drm_atomic_commit(state);
out:
	if (ret == -EDEADLK) {
		drm_atomic_state_clear(state);
		drm_modeset_backoff(&ctx);
		goto retry;
	}
	drm_atomic_state_put(state);
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
	if (ret == 0) {
		gXgCurFb = fb;
		gXgCurCtl[0] = intel_de_read(display, XG_CUR_CTL(0));
		gXgCurCtl[1] = gXgKmsJoined ? intel_de_read(display, XG_CUR_CTL(1)) : 0;
	}
	drm_framebuffer_put(fb);	/* on success the committed plane state holds its own reference */
	snprintf(line, sizeof(line), "kms: C1 cursor plane %s: %d, CUR_CTL A 0x%x B 0x%x, our buffer at GGTT 0x%llx",
	    ret == 0 ? "enabled" : "refused", ret, gXgCurCtl[0], gXgCurCtl[1], gXgCurGgtt);
	xg_disp_line(display, line);
	if (ret == 0 && (gXgCurCtl[0] == 0 || (gXgKmsJoined && gXgCurCtl[1] == 0)))
		ret = -ENODEV;	/* i915 did not turn it on where we need it */
	return (ret);
}

/* the desktop-to-raster scale as a 16.16 fraction (the plane scaler's: the cursor plane is not scaled) */
static unsigned int
xg_kms_cursor_scale_x(void)
{
	return ((unsigned int)(((unsigned long long)gXgKmsCrtc->base.state->mode.hdisplay << 16) / gXgKmsFb->width));
}
static unsigned int
xg_kms_cursor_scale_y(void)
{
	return ((unsigned int)(((unsigned long long)gXgKmsCrtc->base.state->mode.vdisplay << 16) / gXgKmsFb->height));
}

static unsigned int
xg_kms_cursor_image(void *ctx, const unsigned int *argb, unsigned int width, unsigned int height, unsigned int pitch)
{
	unsigned int sx, sy, ow, oh, ox, oy;
	unsigned long fl;
	unsigned int *buf;

	(void)ctx;
	if (!gXgKmsCommitted || gXgKmsFb == NULL || gXgKmsCrtc == NULL)
		return (XG_KMS_ERR_NOT_READY);
	if (width == 0 || argb == NULL) {
		spin_lock_irqsave(&gXgCurLock, fl);
		gXgCurHave = 0;
		xg_kms_cursor_write_locked();
		spin_unlock_irqrestore(&gXgCurLock, fl);
		return (XG_KMS_OK);
	}
	if (height == 0 || width > 512 || height > 512 || pitch < width * 4U)
		return (XG_KMS_ERR_BAD_ARGUMENT);
	sx = xg_kms_cursor_scale_x();
	sy = xg_kms_cursor_scale_y();
	ow = (unsigned int)(((unsigned long long)width * sx + 0xffffU) >> 16);
	oh = (unsigned int)(((unsigned long long)height * sy + 0xffffU) >> 16);
	if (ow == 0 || oh == 0 || ow > XG_CUR_DIM || oh > XG_CUR_DIM)
		return (XG_KMS_ERR_UNSUPPORTED);	/* WindowServer draws this one itself */
	mutex_lock(&gXgKmsModeLock);
	if (!gXgCurOn) {
		int ret = xg_kms_cursor_enable();

		if (ret != 0) {
			mutex_unlock(&gXgKmsModeLock);
			return (XG_KMS_ERR_UNSUPPORTED);
		}
		gXgCurOn = 1;
	}
	mutex_unlock(&gXgKmsModeLock);
	/*
	 * Bilinear, premultiplied: each raster pixel samples the desktop image at its centre.  Written straight into
	 * the buffer the plane scans (one frame may show half of the old shape at a shape change).
	 */
	buf = gXgCurKva;
	for (oy = 0; oy < XG_CUR_DIM; oy++) {
		for (ox = 0; ox < XG_CUR_DIM; ox++) {
			unsigned int out = 0;

			if (ox < ow && oy < oh) {
				/* source position of this pixel's centre, 16.16 */
				const long long fx = ((((long long)ox << 16) + 0x8000) << 16) / sx - 0x8000;
				const long long fy = ((((long long)oy << 16) + 0x8000) << 16) / sy - 0x8000;
				const int x0 = fx < 0 ? 0 : (int)(fx >> 16), y0 = fy < 0 ? 0 : (int)(fy >> 16);
				const int x1 = x0 + 1 < (int)width ? x0 + 1 : x0, y1 = y0 + 1 < (int)height ? y0 + 1 : y0;
				const unsigned int wx = fx < 0 ? 0 : (unsigned int)(fx & 0xffff), wy = fy < 0 ? 0 : (unsigned int)(fy & 0xffff);
				unsigned long long acc[4] = { 0, 0, 0, 0 };
				int k, c;

				for (k = 0; k < 4; k++) {
					const int xx = (k & 1) ? x1 : x0, yy = (k & 2) ? y1 : y0;
					const unsigned int p = (xx < (int)width && yy < (int)height) ?
					    *(const unsigned int *)((const char *)argb + (size_t)yy * pitch + (size_t)xx * 4U) : 0;
					const unsigned int a = p >> 24;
					const unsigned long long w = (unsigned long long)((k & 1) ? wx : 0x10000U - wx) *
					    ((k & 2) ? wy : 0x10000U - wy);

					acc[3] += w * a;
					acc[2] += w * ((((p >> 16) & 255U) * a + 127U) / 255U);
					acc[1] += w * ((((p >> 8) & 255U) * a + 127U) / 255U);
					acc[0] += w * (((p & 255U) * a + 127U) / 255U);
				}
				for (c = 0; c < 4; c++) {
					unsigned long long v = (acc[c] + (1ULL << 31)) >> 32;

					out |= (unsigned int)(v > 255 ? 255 : v) << (8 * c);
				}
			}
			buf[oy * XG_CUR_DIM + ox] = out;
		}
	}
	xg_vm_x_flush(buf, XG_CUR_BYTES);
	__sync_synchronize();
	spin_lock_irqsave(&gXgCurLock, fl);
	gXgCurHave = 1;
	gXgCurImages++;
	xg_kms_cursor_write_locked();
	spin_unlock_irqrestore(&gXgCurLock, fl);
	return (XG_KMS_OK);
}

static unsigned int
xg_kms_cursor_move(void *ctx, int x, int y, unsigned int visible)
{
	unsigned long fl;

	(void)ctx;
	if (!gXgCurOn || gXgKmsFb == NULL || gXgKmsCrtc == NULL)
		return (XG_KMS_ERR_NOT_READY);
	spin_lock_irqsave(&gXgCurLock, fl);
	gXgCurX = (int)(((long long)x * (long long)xg_kms_cursor_scale_x()) >> 16);
	gXgCurY = (int)(((long long)y * (long long)xg_kms_cursor_scale_y()) >> 16);
	gXgCurVis = visible != 0;
	gXgCurMoves++;
	xg_kms_cursor_write_locked();
	spin_unlock_irqrestore(&gXgCurLock, fl);
	return (XG_KMS_OK);
}

static void
xg_kms_check_one(struct intel_display *display, struct intel_crtc *crtc, struct intel_connector *conn,
    struct drm_framebuffer *fb, const struct drm_display_mode *mode, bool force_dsc, int max_bpc, bool commit,
    const char *tag)
{
	struct intel_dp *dp = enc_to_intel_dp(intel_attached_encoder(conn));
	struct intel_plane *primary = to_intel_plane(crtc->base.primary);
	struct drm_modeset_acquire_ctx ctx;
	struct drm_atomic_state *state;
	struct intel_crtc_state *cs;
	struct intel_encoder *encoder;
	bool saved_force_dsc = dp->force_dsc_en;
	int fastset_ok = -1;
	char line[320];
	int ret;

	state = drm_atomic_state_alloc(display->drm);
	if (state == NULL)
		return;
	drm_modeset_acquire_init(&ctx, 0);
	state->acquire_ctx = &ctx;
	to_intel_atomic_state(state)->internal = true;
	dp->force_dsc_en = force_dsc;
retry:
	cs = intel_atomic_get_crtc_state(state, crtc);
	if (IS_ERR(cs)) {
		ret = PTR_ERR(cs);
		goto out;
	}
	ret = drm_atomic_add_affected_planes(state, &crtc->base);
	if (ret)
		goto out;
	if (mode == NULL) {
		cs->uapi.color_mgmt_changed = true;	/* as intel_initial_commit() */
		for_each_intel_encoder_mask(display->drm, encoder, cs->uapi.encoder_mask) {
			if (encoder->initial_fastset_check == NULL)
				continue;
			fastset_ok = encoder->initial_fastset_check(encoder, cs) ? 1 : 0;
			if (!fastset_ok && (ret = drm_atomic_add_affected_connectors(state, &crtc->base)) != 0)
				goto out;
		}
	} else {
		struct drm_connector_state *cn = drm_atomic_get_connector_state(state, &conn->base);

		if (IS_ERR(cn)) {
			ret = PTR_ERR(cn);
			goto out;
		}
		ret = drm_atomic_set_crtc_for_connector(cn, &crtc->base);
		if (ret == 0)
			ret = drm_atomic_set_mode_for_crtc(&cs->uapi, mode);
		if (ret)
			goto out;
		if (max_bpc > 0)
			cn->max_requested_bpc = max_bpc;
		cs->uapi.active = true;
		/*
		 * Always a full modeset.  J1 commits the same raster with the joiner turned on or off; DRM sees an equal
		 * mode, leaves mode_changed clear, and i915 then keeps the old pipe config and refuses the plane (-EINVAL
		 * in ~25 us: a 3840x2400 desktop on one 240 Hz pipe).
		 */
		cs->uapi.mode_changed = true;
	}
	if (fb != NULL) {
		struct drm_plane_state *ps = drm_atomic_get_plane_state(state, &primary->base);
		int w = mode != NULL ? mode->hdisplay : fb->width, h = mode != NULL ? mode->vdisplay : fb->height;

		if (IS_ERR(ps)) {
			ret = PTR_ERR(ps);
			goto out;
		}
		ret = drm_atomic_set_crtc_for_plane(ps, &crtc->base);
		if (ret)
			goto out;
		drm_atomic_set_fb_for_plane(ps, fb);
		ps->src_x = ps->src_y = 0;
		ps->src_w = (u32)fb->width << 16;
		ps->src_h = (u32)fb->height << 16;
		ps->crtc_x = ps->crtc_y = 0;
		ps->crtc_w = w;
		ps->crtc_h = h;
	}
	if (commit) {
		u64 t0 = ktime_get_ns();

		ret = drm_atomic_commit(state);
		gXgKmsLastCommitRet = ret;
		if (ret == 0)
			gXgKmsCommitted = 1;
		snprintf(line, sizeof(line), "kms: commit %s -> %d in %llu us", tag, ret,
		    (unsigned long long)((ktime_get_ns() - t0) / 1000));
		if (ret != -EDEADLK)
			xg_disp_line(display, line);
	} else
		ret = drm_atomic_check_only(state);
out:
	if (ret == -EDEADLK) {
		drm_atomic_state_clear(state);
		drm_modeset_backoff(&ctx);
		goto retry;
	}
	if (ret == 0) {
		struct intel_atomic_state *is = to_intel_atomic_state(state);
		const struct intel_cdclk_state *cd = intel_atomic_get_new_cdclk_state(is);
		const struct drm_connector_state *ncs = drm_atomic_get_new_connector_state(state, &conn->base);
		const struct intel_plane_state *pps = intel_atomic_get_new_plane_state(is, primary);
		const struct skl_ddb_entry *ddb;

		cs = intel_atomic_get_new_crtc_state(is, crtc);
		ddb = &cs->wm.skl.plane_ddb[primary->id];
		snprintf(line, sizeof(line), "kms: %s %s ok: %s initial_fastset %d clock %d kHz port_clock %d lanes %d "
		    "bpp %d max_bpc %d dsc %d bpp_x16 %u slices %u joiner 0x%x psr %d cdclk %d%s plane %s ddb %u..%u",
		    commit ? "committed" : "check", tag, intel_crtc_needs_modeset(cs) ? "modeset" : cs->update_pipe ? "fastset" : "unchanged", fastset_ok,
		    cs->hw.adjusted_mode.crtc_clock, cs->port_clock, cs->lane_count, cs->pipe_bpp,
		    ncs != NULL ? ncs->max_bpc : -1,
		    cs->dsc.compression_enable ? 1 : 0, cs->dsc.compressed_bpp_x16, cs->dsc.slice_count, cs->joiner_pipes,
		    cs->has_psr ? 1 : 0, cd != NULL ? cd->logical.cdclk : display->cdclk.hw.cdclk,
		    cd != NULL ? "" : " (unchanged)",
		    pps == NULL ? "-" : pps->uapi.visible ? "visible" : "off", ddb->start, ddb->end);
	} else
		snprintf(line, sizeof(line), "kms: %s %s failed: %d", commit ? "commit" : "check", tag, ret);
	xg_disp_line(display, line);
	dp->force_dsc_en = saved_force_dsc;
	drm_atomic_state_put(state);
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

/* every input of the bpp / link-rate choice, as intel_dp_compute_config_limits() and intel_dp_max_bpp() see them */
static void
xg_kms_diag(struct intel_display *display, struct intel_connector *conn)
{
	struct intel_dp *dp = enc_to_intel_dp(intel_attached_encoder(conn));
	const struct drm_display_info *info = &conn->base.display_info;
	char line[320];
	int i, n = 0;

	snprintf(line, sizeof(line), "kms: diag edid_bpc %d max_requested_bpc %d vbt_edp_bpp %d use_max_params %d "
	    "edp_rev 0x%02x force_rate %d force_lanes %d force_dsc %d enable_psr %d",
	    info->bpc, conn->base.state != NULL ? conn->base.state->max_requested_bpc : -1, conn->panel.vbt.edp.bpp,
	    dp->use_max_params ? 1 : 0, dp->edp_dpcd[0], dp->link.force_rate, dp->link.force_lane_count,
	    dp->force_dsc_en ? 1 : 0, display->params.enable_psr);
	xg_disp_line(display, line);
	n = snprintf(line, sizeof(line), "kms: diag common_rates %d:", dp->num_common_rates);
	for (i = 0; i < dp->num_common_rates && n < (int)sizeof(line) - 12; i++)
		n += snprintf(line + n, sizeof(line) - n, " %d", intel_dp_common_rate(dp, i));
	n += snprintf(line + n, sizeof(line) - n, " max_rate %d max_lanes %d", intel_dp_max_link_rate(dp),
	    intel_dp_max_lane_count(dp));
	xg_disp_line(display, line);
	{
		u8 bpcs[DP_DSC_RECEIVER_CAP_SIZE];
		int nb = drm_dp_dsc_sink_supported_input_bpcs(conn->dp.dsc_dpcd, bpcs);

		snprintf(line, sizeof(line), "kms: diag dsc sink %d slices %u input_bpcs %d (max %u) edp_out_bpp_x16 %u vbt_dsc %d",
		    drm_dp_sink_supports_dsc(conn->dp.dsc_dpcd) ? 1 : 0,
		    drm_dp_dsc_sink_max_slice_count(conn->dp.dsc_dpcd, true), nb, nb > 0 ? bpcs[0] : 0,
		    drm_edp_dsc_sink_output_bpp(conn->dp.dsc_dpcd), conn->panel.vbt.edp.dsc_disable ? 0 : 1);
	}
	xg_disp_line(display, line);
}

void
xg_display_kms_check(struct intel_display *display)
{
	struct drm_i915_private *i915 = to_i915(display->drm);
	struct drm_connector_list_iter it;
	struct drm_connector *dc;
	struct intel_connector *edp = NULL;
	struct intel_crtc *crtc, *active = NULL;
	struct drm_framebuffer *fb = NULL;
	struct drm_mode_fb_cmd2 cmd = {};
	struct drm_i915_gem_object *obj;
	const struct drm_display_mode *m;
	const struct drm_display_mode *booted = NULL;
	unsigned long debug = __drm_debug;
	char tag[48];

	for_each_intel_crtc(display->drm, crtc)
		if (active == NULL && to_intel_crtc_state(crtc->base.state)->hw.active)
			active = crtc;
	drm_connector_list_iter_begin(display->drm, &it);
	drm_for_each_connector_iter(dc, &it)
		if (edp == NULL && dc->connector_type == DRM_MODE_CONNECTOR_eDP)
			edp = to_intel_connector(dc);
	drm_connector_list_iter_end(&it);
	if (xg_display_external_dp()) {
		unsigned int connected = 0, active_count = 0;
		active = NULL;
		edp = NULL;
		for_each_intel_crtc(display->drm, crtc)
			if (to_intel_crtc_state(crtc->base.state)->hw.active)
				active_count++;
		mutex_lock(&display->drm->mode_config.mutex);
		drm_connector_list_iter_begin(display->drm, &it);
		drm_for_each_connector_iter(dc, &it) {
			if (dc->connector_type != DRM_MODE_CONNECTOR_DisplayPort)
				continue;
			if (to_intel_connector(dc)->mst_port != NULL ||
			    drm_helper_probe_single_connector_modes(dc, 3840, 2160) <= 0 ||
			    dc->status != connector_status_connected || dc->state == NULL || dc->state->crtc == NULL)
				continue;
			if (intel_attached_dp(to_intel_connector(dc))->is_mst)
				continue;
			connected++;
			edp = to_intel_connector(dc);
			active = to_intel_crtc(dc->state->crtc);
		}
		drm_connector_list_iter_end(&it);
		mutex_unlock(&display->drm->mode_config.mutex);
		/* The inherited accel flips pipe A directly; do not drive another pipe by guessing. */
		if (connected != 1 || active_count != 1 || active == NULL || active->pipe != PIPE_A ||
		    !to_intel_crtc_state(active->base.state)->hw.active || xg_kms_preferred_mode(edp) == NULL) {
			xg_disp_line(display, "kms: external DP requires one firmware-active pipe-A SST output and valid EDID modes");
			return;
		}
	}
	if (active == NULL || edp == NULL) {
		xg_disp_line(display, active == NULL ? "kms: check skipped: no active pipe" : "kms: check skipped: no eDP");
		return;
	}
	xg_kms_diag(display, edp);

	/* the adapter's scanout geometry; K2 commits with this, the accel keeps writing PLANE_SURF over it */
	m = xg_display_external_dp() ? xg_kms_preferred_mode(edp) : NULL;
	cmd.width = m != NULL ? (unsigned int)m->hdisplay : 2560U;
	cmd.height = m != NULL ? (unsigned int)m->vdisplay : 1600U;
	cmd.pixel_format = DRM_FORMAT_XRGB8888;
	cmd.pitches[0] = cmd.width * 4U;
	/* sized for the largest scaled desktop (A1), so every placeholder size is a framebuffer over the same pages */
	obj = i915_gem_object_create_shmem(i915, PAGE_ALIGN(XG_KMS_PLANE_MAX_W * 4U * XG_KMS_PLANE_MAX_H));
	if (IS_ERR(obj))
		xg_disp_line(display, "kms: placeholder fb: shmem object failed; checking with the plane off");
	else {
		fb = intel_framebuffer_create(intel_bo_to_drm_bo(obj), &cmd);
		if (xg_display_kms_level() >= 3 && !IS_ERR(fb))
			gXgKmsObj = obj;	/* keep our reference for setPlane */
		else
			i915_gem_object_put(obj);
		if (IS_ERR(fb)) {
			xg_disp_line(display, "kms: placeholder fb: framebuffer create failed; checking with the plane off");
			fb = NULL;
		}
	}

	/* drm.debug only for the K2a checks: at kms=3 its output made the boot commit take 12.7 s instead of 1 s */
	if (xg_display_kms_level() == 2)
		__drm_debug |= BIT(DRM_UT_KMS) | BIT(DRM_UT_ATOMIC);
	/* K2 boots skip the checks: K2a.3 answered them, and with drm.debug on they cost ~30 s of stage 4's budget */
	if (xg_display_kms_level() == 2)
		xg_kms_check_one(display, active, edp, fb, NULL, false, 0, false, "readout");
	/*
	 * K2a.2 showed why every mode came out as HBR3 x4 at 18 bpp: the VBT declares seamless DRRS, so
	 * intel_dp_mode_clock() sizes the link for the panel's highest fixed mode (300 Hz, 1.387 GHz) and a refresh change
	 * is only an M/N update -- and 300 Hz uncompressed only fits HBR3 x4 at 6 bpc.  K2a.3 checks the plan for K2/K3
	 * with DRRS off (each mode sized for itself, a full modeset per refresh change, as NVKMS does): every fixed mode
	 * as i915 would pick it, at 8 bpc (the firmware's and NVKMS's input depth), and the high refresh rates with DSC.
	 */
	xg_disp_line(display, edp->panel.vbt.drrs_type == DRRS_TYPE_SEAMLESS ?
	    "kms: VBT DRRS seamless -> off for the K2a.3 checks" : "kms: VBT DRRS not seamless");
	edp->panel.vbt.drrs_type = DRRS_TYPE_NONE;
	list_for_each_entry(m, xg_kms_mode_list(edp), head) {
		if (!xg_kms_mode_allowed(m))
			continue;
		if (xg_display_kms_level() != 2)
			break;
		int hz = drm_mode_vrefresh(m);

		snprintf(tag, sizeof(tag), "%dx%d@%d", m->hdisplay, m->vdisplay, hz);
		xg_kms_check_one(display, active, edp, fb, m, false, 0, false, tag);
		snprintf(tag, sizeof(tag), "%dx%d@%d/8bpc", m->hdisplay, m->vdisplay, hz);
		xg_kms_check_one(display, active, edp, fb, m, false, 8, false, tag);
		if (!xg_display_external_dp() && hz > 60) {
			snprintf(tag, sizeof(tag), "%dx%d@%d/8bpc+dsc", m->hdisplay, m->vdisplay, hz);
			xg_kms_check_one(display, active, edp, fb, m, true, 8, false, tag);
		}
	}
	/*
	 * K2 (kms=3): the first commit of i915's own, where intel_initial_commit() would make it -- before the adapter
	 * publishes its nub.  A full modeset onto the panel's preferred mode (60 Hz) with what K2a.3 checked: DRRS off,
	 * 8 bpc (2.16 GHz x4, 24 bpp, no DSC), PSR off, the placeholder framebuffer on the primary plane (prepare_fb pins
	 * it into the GGTT; its pages are zero, so the panel shows black until the first flip).  The accel then writes
	 * PLANE_SURF_1_A over it exactly as it wrote over the firmware's plane: same format, size and stride, and the
	 * watermarks/DDB are now i915's.  NVKMS takes the panel the same way (docs/31 §31.4: black takeover, then a full
	 * modeset at 60 Hz).  Nothing commits without a placeholder fb: the plane would come back off.
	 */
	if (xg_display_kms_level() >= 3) {
		m = xg_kms_preferred_mode(edp);
		if (fb == NULL || m == NULL)
			xg_disp_line(display, fb == NULL ? "kms: commit skipped: no placeholder fb" :
			    "kms: commit skipped: no fixed mode");
		else {
			unsigned int bw, bh;
			const struct drm_display_mode *want = xg_kms_boot_pick(edp, m, &bw, &bh);

			/*
			 * K4: straight into the mode the adapter last set -- the refresh WindowServer will ask for and the
			 * desktop on the plane, joined when one pipe cannot carry it -- so the desktop comes up without the
			 * 60 Hz commit, a refresh modeset and a joiner modeset after it.  The same rules as K3/J1 (8 bpc, DSC
			 * above 60 Hz, PSR off).  A refused commit leaves the pipe as it was; the 60 Hz commit follows.
			 */
			if (want != NULL && (want != m || bw != fb->width || bh != fb->height)) {
				struct drm_mode_fb_cmd2 bcmd = {};
				struct drm_framebuffer *bfb = fb;
				int hz = drm_mode_vrefresh(want);

				if (bw != fb->width || bh != fb->height) {
					bcmd.width = bw;
					bcmd.height = bh;
					bcmd.pixel_format = DRM_FORMAT_XRGB8888;
					bcmd.pitches[0] = bw * 4U;
					bfb = gXgKmsObj != NULL ? intel_framebuffer_create(intel_bo_to_drm_bo(gXgKmsObj), &bcmd) :
					    ERR_PTR(-ENOENT);
				}
				if (IS_ERR(bfb))
					xg_disp_line(display, "kms: K4 boot mode: no placeholder at the saved desktop; 60 Hz");
				else {
					gXgKmsDisplay = display;	/* xg_kms_needs_joiner() reads its limits */
					edp->force_joined_pipes = xg_kms_needs_joiner(want, bw, bh) ? 2 : 0;
					snprintf(tag, sizeof(tag), "K4 %dx%d@%d desktop %ux%u%s", want->hdisplay, want->vdisplay, hz,
					    bw, bh, edp->force_joined_pipes ? "+joiner" : "");
					gXgKmsLastCommitRet = -EINVAL;
					xg_kms_check_one(display, active, edp, bfb, want, !xg_display_external_dp() && hz > 60, 8, true, tag);
					if (gXgKmsLastCommitRet == 0) {
						if (bfb != fb) {
							drm_framebuffer_put(fb);
							fb = bfb;
						}
						booted = want;
						gXgBootSaved = 1;
					} else {
						if (bfb != fb)
							drm_framebuffer_put(bfb);
						edp->force_joined_pipes = 0;
						xg_disp_line(display, "kms: K4 boot mode refused; the panel's preferred mode instead");
					}
				}
			}
			if (booted == NULL) {
				snprintf(tag, sizeof(tag), "%dx%d@%d/8bpc", m->hdisplay, m->vdisplay, drm_mode_vrefresh(m));
				xg_kms_check_one(display, active, edp, fb, m, false, 8, true, tag);
				booted = m;
			}
		}
	}
	__drm_debug = debug;
	if (xg_display_kms_level() >= 3 && gXgKmsCommitted) {
		xg_kms_capture_fixed_modes(display, active, edp, fb, booted);
		/*
		 * K3: setMode runs on the adapter's thread.  i915 lets only the probe thread touch the display until
		 * intel_display_driver_register() opens it to all, and K1 skips register; open it here instead.
		 */
		intel_display_driver_enable_user_access(display);
		xg_kms_update_joiner();
		wmb();
		WRITE_ONCE(gXgKmsModesReady, 1);
	}
	if (fb != NULL)
		drm_framebuffer_put(fb);	/* a committed plane state holds its own reference */
	if (xg_display_kms_level() >= 3)
		xg_display_kms_dump(display);	/* the crtc as i915 now knows it, after the commit */
}
