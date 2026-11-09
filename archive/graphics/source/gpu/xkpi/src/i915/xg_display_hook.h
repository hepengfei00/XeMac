/* SPDX-License-Identifier: MIT */
/*
 * XeGfx display bring-up hook, step M1a: read the VBT that intel_bios_init()
 * parsed, with the display engine left alone.  Implemented in xg_display_hook.c.
 *
 * The level comes from compat.linuxkpi.i915_xegfx_disp_level (set through
 * debug.xegfx_env by the bring-up script): 0 = current behaviour (display left
 * alone), 1 = M1a read-only VBT dump.
 */
#ifndef _XG_DISPLAY_HOOK_H_
#define _XG_DISPLAY_HOOK_H_

struct intel_display;

int xg_display_disp_level(void);
void xg_display_set_external_dp(int enabled);
int xg_display_external_dp(void);
int xg_display_keep_dp_child(unsigned int handle, unsigned int port);
int xg_display_dvo_port(struct intel_display *display, unsigned int dvo_port);
void xg_display_dump_vbt(struct intel_display *display);
void xg_display_dump_bios(struct intel_display *display);
void xg_display_note(struct intel_display *display, const char *line);
void xg_display_nub_publish(int child_devices, int bdb_blocks);
int xg_display_dump_text(char *out, int max);

/* M2c: raw capture published through the display nub (read-only; no display register). */
#define XG_DISPLAY_MAX_CHILDREN		8
#define XG_DISPLAY_MAX_CHILD_SIZE	64
#define XG_DISPLAY_MAX_VBT		8192
void xg_display_set_vbt(const void *data, unsigned int len);
unsigned int xg_display_vbt_get(const void **data);
void xg_display_add_child(const void *data, unsigned int len);
int xg_display_children_get(const void **data, unsigned int *child_size);

/*
 * Panel identity read over the internal panel's AUX channel (read-only DPCD + EDID through I2C-over-AUX), captured
 * before the nub is published.  DPCD is five 16-byte regions in this order: 0x000, 0x060 (DSC), 0x070 (PSR),
 * 0x2200 (extended receiver caps), 0x700 (eDP caps).  A failed read leaves the EDID empty.
 */
#define XG_DISPLAY_MAX_EDID		512
#define XG_DISPLAY_DPCD_BYTES		80
void xg_display_aux_set_display(struct intel_display *display);
int xg_display_aux_probe(void);
unsigned int xg_display_edid_get(const void **data);
unsigned int xg_display_dpcd_get(const void **data);

/*
 * Staged eDP takeover (xg_display_hook.c): 0 = display left alone, 1 = K1 build outputs + read out, commit nothing,
 * 2 = K2a as K1 plus an atomic check (never a commit) of the read-out state and of each panel fixed mode, with a
 * placeholder framebuffer on the primary plane (K2a.2); the fixed modes are checked with DRRS off, at the default
 * and at 8 bpc, and above 60 Hz with DSC forced (K2a.3).  Leaves the connector's VBT DRRS type at none.
 * 3 = K2: the checks, then one full modeset onto the preferred 60 Hz mode at 8 bpc with the placeholder framebuffer.
 */
int xg_display_kms_level(void);
void xg_display_kms_dump(struct intel_display *display);
void xg_display_kms_check(struct intel_display *display);
/* J1: whether pipe A is joined with pipe B, pipe B's PLANE_SURF minus pipe A's, and a generation counter */
int xg_display_kms_joiner_info(unsigned int *active, unsigned int *delta, unsigned int *gen);

/*
 * Hardware colour (native-display docs/37-RENDER-COMPRESSION.md §37.28): the colour state WindowServer attaches to its
 * display-pipe transactions, as the accelerator reads it (IOAccelDisplayPipeTransaction2), for the pipe's pre-CSC
 * degamma LUT, CSC and post-CSC gamma LUT.  Values are IEEE-754 single-precision bit patterns: the kernel has no
 * floating point, so they are converted with integer arithmetic.  The accelerator keeps the same layout.
 */
#define XG_PIPE_COLOUR_VERSION	1
#define XG_PIPE_COLOUR_MAX	4096
#define XG_PIPE_COLOUR_PRE	0x1u	/* pre[] holds npre RGB entries: the linearization (pipe pre-gamma table) */
#define XG_PIPE_COLOUR_POST	0x2u	/* post[] holds npost RGB entries: the encoding (pipe post-gamma table) */
#define XG_PIPE_COLOUR_MATRIX	0x4u	/* matrix[] holds the colour matrix */
struct xg_pipe_colour {
	unsigned int	version;
	unsigned int	flags;
	unsigned int	npre, npost;
	unsigned int	matrix_mode;	/* bit 0: 0 = 3x4 row-major, offsets in column 3; 1 = 3x3 row-major, then 3 offsets.
					 * bit 1: transposed */
	unsigned int	matrix[12];
	unsigned int	pre[3 * XG_PIPE_COLOUR_MAX];
	unsigned int	post[3 * XG_PIPE_COLOUR_MAX];
};
int xg_display_kms_colour(const struct xg_pipe_colour *c, unsigned int *us_out);

/*
 * M3b: read-only KMS KAPI v1.
 *
 * The KAPI is a static, fully const function table in this kext.  The nub
 * publishes its address as an 8-byte OSData ("xegfx-kms-kapi"); see
 * docs/11-M3B-KAPI.md in the native-display project for the ABI.  The table
 * lives for as long as the kext is loaded and contains no state of its own:
 * the read-only entry points consult the VBT/child snapshot captured by
 * intel_bios_init() and the panel timing handed over by the NVRM AGDC
 * producer.
 */
#define XG_KMS_KAPI_MAGIC	0x58474b4dU	/* table magic */
#define XG_KMS_KAPI_VERSION	1

/*
 * M3d additive scanout extension (v1.1, same 136-byte table and version 1).
 *
 * The three entry points sit in the first three 8-byte words of the v1
 * table's reserved[8] area (offsets 64, 72 and 80); reserved[3..7] stay zero.
 * XG_KMS_KAPI_FLAG_SCANOUT tells a reader that those words are function
 * pointers.  A reader that only knows v1 must ignore the reserved area, which
 * is what the existing getTiming/getConnectorInfo path does.
 *
 * allocScanout(ctx, width, height, bytes_per_pixel, flags, out)
 * freeScanout(ctx, handle)
 * getScanoutInfo(ctx, handle, out)
 */
#define XG_KMS_KAPI_FLAG_SCANOUT	(1U << 1)

#define XG_KMS_SCANOUT_MAX		2U
#define XG_KMS_SCANOUT_MAX_WIDTH	5120U
#define XG_KMS_SCANOUT_MAX_HEIGHT	2880U
#define XG_KMS_SCANOUT_MAX_BYTES	(64ULL * 1024 * 1024)
#define XG_KMS_SCANOUT_FLAG_ZERO	(1U << 0)	/* accepted; memory is always zeroed */
#define XG_KMS_SCANOUT_BYTES		48U

/*
 * 48-byte descriptor, layout-compatible with the adapter's
 * XeGfxKmsScanoutInfo.  phys is host physical (below 4 GiB because the
 * adapter's xgGetScanoutInfo returns a u32); kva is the CPU mapping; length
 * is the page-rounded allocation; surface_id is the opaque handle to pass to
 * getScanoutInfo/freeScanout; reserved carries the low 32 bits of the GGTT
 * offset (all of it: the allocator inserts below 4 GiB), or 0.
 */
struct xg_kms_scanout_info {
	unsigned long long	phys;		/*  0 */
	void			*kva;		/*  8 */
	unsigned long long	length;		/* 16 */
	unsigned int		pitch;		/* 24: bytes per row, 256-byte aligned */
	unsigned int		width;		/* 28 */
	unsigned int		height;		/* 32 */
	unsigned int		bpp;		/* 36: bits per pixel */
	unsigned int		surface_id;	/* 40: opaque handle for get/free */
	unsigned int		reserved;	/* 44: GGTT offset, 0 when not mapped */
};

const void *xg_display_kms_kapi(void);
unsigned int xg_display_kms_kapi_size(void);
unsigned int xg_display_kms_kapi_version(void);
void xg_display_set_measured_timing(const void *data, unsigned int len);
/* M3d diagnostic; allocates only when explicitly invoked (debug.xegfx_kms_scanout). */
int xg_display_kms_scanout_selftest(char *out, int max);

#endif /* _XG_DISPLAY_HOOK_H_ */

