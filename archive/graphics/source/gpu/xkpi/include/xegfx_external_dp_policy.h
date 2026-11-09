/* SPDX-License-Identifier: MIT */
#ifndef XEGFX_EXTERNAL_DP_POLICY_H
#define XEGFX_EXTERNAL_DP_POLICY_H

/* Display v14: VBT F/G/H map to the USBC1/2/3 register banks, including legacy DP. */
static inline unsigned int xg_dp_aux_ctl(unsigned int aux)
{
	switch (aux) {
	case 0x40U: return 0x64010U;
	case 0x10U: return 0x64110U;
	case 0x60U: return 0x16f210U;
	case 0x70U: return 0x16f410U;
	case 0x80U: return 0x16f610U;
	default: return 0;
	}
}

static inline int xg_dp_vbt_candidate(unsigned int type, unsigned int aux,
    unsigned int type_c, unsigned int thunderbolt)
{
	return (type & (1U << 2)) && !(type & (1U << 12)) &&
	    !type_c && !thunderbolt && xg_dp_aux_ctl(aux) != 0;
}

/* Initial external-DP scope: progressive EDID modes through 4K60 SDR. */
static inline int xg_dp_mode_accepted(unsigned int w, unsigned int h,
    unsigned int hz, int interlaced_or_doublescan)
{
	return w >= 640U && w <= 3840U && h >= 400U && h <= 2160U &&
	    !(w & 15U) && hz >= 24U && hz <= 60U && !interlaced_or_doublescan;
}

#endif
