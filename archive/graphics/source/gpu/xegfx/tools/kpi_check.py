#!/usr/bin/env python3
# Usage: tools/kpi_check.py <kext> : every undefined symbol must come from a library the kext declares in
# OSBundleLibraries, never from com.apple.kpi.private (Apple-internal). Prints the offenders, exit 1 if any.
import os, plistlib, re, subprocess, sys
kext = sys.argv[1]
decl = set(plistlib.load(open(kext + "/Contents/Info.plist", "rb"))["OSBundleLibraries"]) - {"com.apple.kpi.private"}
exe = kext + "/Contents/MacOS/" + plistlib.load(open(kext + "/Contents/Info.plist", "rb"))["CFBundleExecutable"]
und = {l.split()[-1] for l in subprocess.run(["nm", "-u", exe], capture_output=True, text=True).stdout.split("\n") if l.strip()}
report = os.environ.get("XEGFX_KPI_REPORT")
out = open(report).read() if report else subprocess.run(["kmutil", "libraries", "-p", kext, "--undef-symbols"], capture_output=True, text=True).stdout
ok, cur = set(), None
for line in out.splitlines():
    m = re.match(r"^\s+(_\S+) in (multiple:|/\S+: (\S+) \()", line)
    if m:
        cur = m.group(1)
        if m.group(3) in decl:
            ok.add(cur)
        continue
    m = re.match(r"^\s+/\S+: (\S+) \(", line)
    if m and cur and m.group(1) in decl:
        ok.add(cur)
bad = sorted(und - ok)
if bad:
    print("not resolvable from the declared, public libraries: " + " ".join(bad))
    sys.exit(1)
print(f"kpi check ok ({len(und)} symbols)")
