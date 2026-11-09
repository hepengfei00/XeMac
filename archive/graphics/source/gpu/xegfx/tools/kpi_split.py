#!/usr/bin/env python3
# Splits out/undefined.txt into symbols the macOS kernel already exports (with the bundle that exports them) and the
# ones XeGfx must implement itself. Uses Bash's kcabi reader on the boot kernel collection.
import os, sys
sys.path.insert(0, os.path.expanduser(os.environ.get("XEGFX_KCABI", "~/src/kcabi")))
import kcfile, linkcheck
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
kc = kcfile.KC(os.environ.get("KCABI_KC", "/System/Library/KernelCollections/BootKernelExtensions.kc"))
exp = linkcheck.exported(kc)
# XNU exports some names FreeBSD also uses with another ABI (eventhandler_*, selrecord, ...): a name binds to XNU
# only when tools/xnu-allow.txt lists it as checked
allow = {l.strip() for l in open(os.path.join(ROOT, "tools/xnu-allow.txt")) if l.strip()}
have, todo, clash = [], [], []
for line in open(os.path.join(ROOT, "out/undefined.txt")):
    n, sym = line.split()
    if sym in exp and sym[1:] in allow:
        have.append((int(n), sym, exp[sym]))
    elif sym in exp and not sym[1:].startswith("xg_"):
        clash.append((int(n), sym, exp[sym]))
    else:
        todo.append((int(n), sym, ""))
open(os.path.join(ROOT, "out/kpi-clash.txt"), "w").write("".join(f"{s} {o}\n" for _, s, o in clash))
if clash:
    print("ABI CHECK NEEDED (exported by XNU, not on tools/xnu-allow.txt): " + " ".join(s for _, s, _ in clash))
open(os.path.join(ROOT, "out/kpi-have.txt"), "w").write("".join(f"{s} {o}\n" for _, s, o in have))
open(os.path.join(ROOT, "out/kpi-todo.txt"), "w").write("".join(f"{n} {s}\n" for n, s, _ in todo))
print(f"exported by macOS: {len(have)}   to implement: {len(todo)}")
