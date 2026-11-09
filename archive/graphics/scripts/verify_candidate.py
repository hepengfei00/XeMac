#!/usr/bin/env python3
"""Exercise the real AUX-selection wrapper with synthetic VBT/AUX responses."""
from pathlib import Path
import hashlib
import json
import plistlib
import subprocess

ROOT = Path(__file__).resolve().parent
CORE = ROOT / 'core'
OUT = ROOT / 'validation'
OUT.mkdir(exist_ok=True)
hook = (CORE / 'gpu/xkpi/src/i915/xg_display_hook.c').read_text()


def function(signature):
    start = hook.index(signature)
    opening = hook.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (hook[end] == '{') - (hook[end] == '}')
        end += 1
    return hook[start:end]


prefix = r'''
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "xegfx_external_dp_policy.h"
#define XG_DISPLAY_MAX_EDID 512
#define XG_DISPLAY_DPCD_BYTES 80
static unsigned char gXgChildData[8 * 40], gXgEdid[512], gXgDpcd[80];
static unsigned int gXgEdidLen, gXgChildStride = 40, gXgChildSize = 40;
static int gXgChildCount, gXgDpChildIndex = -1, gXgDpcdOk, external;
static void *gXgAuxDisplay = (void *)1;
static char gXgAuxWhy[128];
static unsigned int responding, calls;
static int active_raw_port = -1;
static int xg_aux_child_is_active(const unsigned char *c) { return active_raw_port < 0 || c[16] == active_raw_port; }
static int xg_display_external_dp(void) { return external; }
static void xg_disp_line(void *p, const char *s) { (void)p; (void)s; }
static unsigned int xg_kms_vbt_u16(const unsigned char *p, unsigned int n) {
    return p[n] | ((unsigned int)p[n + 1] << 8);
}
static int xg_display_aux_probe_child(const unsigned char *c) {
    calls++; gXgEdidLen = 0; gXgDpcdOk = 0;
    if (!c) return -EIO;
    unsigned int mask = c[25] == 0x40 ? 1U : c[25] == 0x10 ? 2U :
        c[25] == 0x60 ? 4U : c[25] == 0x70 ? 8U : c[25] == 0x80 ? 16U : 0;
    if (!(responding & mask)) return -EIO;
    memset(gXgEdid, c[0], sizeof gXgEdid);
    memset(gXgDpcd, c[25], sizeof gXgDpcd);
    gXgEdidLen = 128; gXgDpcdOk = 1;
    return 0;
}
static void reset(void) {
    memset(gXgChildData, 0, sizeof gXgChildData);
    external = 1; responding = calls = 0; active_raw_port = -1;
    gXgChildCount = 0; gXgChildSize = 40;
    gXgEdidLen = 0; gXgDpcdOk = 0; gXgDpChildIndex = -1;
}
static void child(unsigned int index, unsigned int type, unsigned int aux,
    unsigned int tc) {
    unsigned char *c = gXgChildData + index * 40;
    c[0] = (unsigned char)(index + 1); c[2] = type; c[3] = type >> 8;
    c[16] = index + 7; c[25] = aux; c[33] = tc;
    gXgChildCount = index + 1;
}
'''
prefix += 'static const unsigned char z890m_vbt[160] = {64,0,198,104,0,16,16,0,179,27,16,0,0,0,0,0,13,0,0,3,0,0,0,192,0,96,1,32,1,0,0,0,0,3,0,0,0,0,0,254,32,0,198,104,0,16,16,0,207,27,16,0,0,0,0,0,15,0,0,4,0,0,0,192,0,112,1,32,1,0,0,0,0,3,0,0,0,0,0,254,16,0,214,96,0,16,16,0,235,27,16,0,0,0,0,0,17,0,0,5,0,0,0,192,0,128,1,32,1,0,0,0,0,0,0,0,0,0,5,254,2,0,210,96,0,16,16,0,7,28,16,0,0,0,0,0,20,0,0,6,0,0,0,192,0,0,1,32,1,0,0,0,0,0,0,0,0,0,5,254};\n'
main = r'''
int main(void) {
    /* Exact model target and scanout storage size: 33,177,600, not 8,294,400 bytes. */
    assert(xg_dp_mode_accepted(3840, 2160, 60, 0));
    assert(3840ULL * 2160 * 4 == 33177600ULL);
    assert(xg_dp_mode_accepted(1920, 1080, 60, 0));
    assert(!xg_dp_mode_accepted(3840, 2160, 120, 0));
    assert(!xg_dp_mode_accepted(3840, 2160, 60, 1));
    assert(!xg_dp_mode_accepted(5120, 2880, 60, 0));
    assert(!xg_dp_mode_accepted(3839, 2160, 60, 0));
    assert(!xg_dp_mode_accepted(0, 0, 0, 0));
    /* A VBT internal child before the real DP must not capture the console. */
    reset(); child(0, 0x1004, 0x40, 0); child(1, 0x0004, 0x10, 0);
    responding = 2;
    assert(xg_display_aux_probe() == 0 && gXgDpChildIndex == 1 && calls == 1);
    assert(xg_display_keep_dp_child(2, 8));
    assert(!xg_display_keep_dp_child(1, 7));
    assert(gXgEdidLen == 128 && gXgEdid[0] == 2 && gXgDpcd[0] == 0x10);
    /* A later disconnected connector must not erase the selected sink's EDID. */
    reset(); child(0, 4, 0x40, 0); child(1, 4, 0x10, 0); responding = 1;
    assert(xg_display_aux_probe() == 0 && gXgDpChildIndex == 0 && calls == 2);
    assert(gXgEdidLen == 128 && gXgEdid[0] == 1 && gXgDpcd[0] == 0x40);
    /* Ambiguous/missing sinks fail closed, even after a previous successful probe. */
    responding = 3;
    assert(xg_display_aux_probe() < 0 && gXgDpChildIndex == -1 && !gXgEdidLen && !gXgDpcdOk);
    responding = 0;
    assert(xg_display_aux_probe() < 0 && gXgDpChildIndex == -1);
    /* Never issue raw combo-PHY AUX transactions to Type-C/TBT/unknown channels. */
    reset(); child(0, 4, 0x40, 1); child(1, 4, 0x10, 2); child(2, 4, 0x20, 0);
    responding = 3;
    assert(xg_display_aux_probe() < 0 && calls == 0);
    /* Truncated VBT records cannot expose AUX/channel flags past their bounds. */
    reset(); child(0, 4, 0x40, 0); gXgChildSize = 25; responding = 1;
    assert(xg_display_aux_probe() < 0 && calls == 0);
    /* Explicitly disabled external profile preserves internal-panel routing. */
    reset(); external = 0; child(0, 4, 0x10, 0); child(1, 0x1004, 0x40, 0);
    responding = 1;
    assert(xg_display_aux_probe() == 0 && calls == 1 && gXgEdid[0] == 2);
    /* Actual Z890M VBT: F/G are Type-C/TBT, H is the legacy DP port. */
    reset(); memcpy(gXgChildData, z890m_vbt, sizeof z890m_vbt); gXgChildCount = 4;
    responding = 16U; active_raw_port = 17;
    assert(xg_dp_aux_ctl(0x80) == 0x16f610U);
    assert(xg_display_aux_probe() == 0 && gXgDpChildIndex == 2 && calls == 1);
    assert(xg_display_keep_dp_child(0x10, 17));
    /* A firmware-inactive legacy DP bank must never receive an AUX transaction. */
    reset(); child(0, 0x60d6, 0x80, 0); responding = 16U; active_raw_port = 17;
    assert(xg_display_aux_probe() < 0 && calls == 0);
    puts("PASS: 11 routing/mode scenarios, including actual Z890M VBT");
    return 0;
}
'''
source = OUT / 'routing-test.c'
source.write_text(prefix + '\n' + function('int xg_display_keep_dp_child(') + '\n' +
                  function('int\nxg_display_aux_probe(void)') + '\n' + main)
binary = OUT / 'routing-test'
compile_result = subprocess.run([
    'xcrun', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
    '-fsanitize=address,undefined', '-g',
    '-I', str(CORE / 'gpu/xkpi/include'), str(source), '-o', str(binary)
], capture_output=True, text=True)
(OUT / 'routing-compile.log').write_text(compile_result.stdout + compile_result.stderr)
compile_result.check_returncode()
run = subprocess.run([str(binary)], capture_output=True, text=True)
(OUT / 'routing-test.log').write_text(run.stdout + run.stderr)
run.check_returncode()
report = {
    'routing_test': 'pass', 'sanitizers': ['address', 'undefined'],
    'tested_source_sha256': hashlib.sha256(hook.encode()).hexdigest(),
    'core_full_build': 'see validation/current-verdict.json',
    'hardware_test': 'latest AUX-H revision not run; initial revision reached stage 4', 'installed': False,
}
(OUT / 'summary.json').write_text(json.dumps(report, indent=2) + '\n')
print(run.stdout.strip())
