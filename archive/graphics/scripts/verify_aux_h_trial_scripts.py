#!/usr/bin/env python3
"""Run the actual arm tail in a temporary directory with hardware actions mocked."""
from pathlib import Path
import os
import plistlib
import subprocess
import tempfile
import json

ROOT = Path(__file__).resolve().parent
source = (ROOT / 'trial-aux-h-20261108/prepare-and-confirm.sh').read_text()
tail = source[source.index('committed=0\n'):]
assert '\nPLIST\n' in tail
results = {}
for mode in ['success', 'collection_failure', 'nvram_failure', 'shutdown_failure']:
    with tempfile.TemporaryDirectory(prefix='wtl arm test ') as d:
        work = Path(d)
        (work / 'stable-config.plist').write_text('stable')
        (work / 'trial-config.plist').write_text('trial')
        (work / 'config.plist').write_text('stable')
        (work / 'restore.sh').write_text('''#!/bin/bash
set -eu
cd "$(dirname "$0")"
cp stable-config.plist config.plist
touch restored
''')
        prefix = r'''
set -euo pipefail
root=$TEST_ROOT
job="$root/job.plist"
nvram() { [ "$TEST_MODE" != nvram_failure ]; }
write_collection() { [ "$TEST_MODE" != collection_failure ]; }
mount_esp() { esp_config="$root/config.plist"; }
write_config() { cp "$1" "$esp_config"; }
unmount_esp() { :; }
chown() { :; }
sync() { :; }
sleep() { :; }
shutdown() {
    if [ "$TEST_MODE" = shutdown_failure ]; then return 1; fi
    printf 'restart\n' >> "$root/restarts"
}
'''
        env = dict(os.environ, TEST_ROOT=d, TEST_MODE=mode, LC_ALL='C')
        run = subprocess.run(['/bin/bash'], input=prefix + tail, text=True,
                             capture_output=True, env=env)
        if mode == 'success':
            assert run.returncode == 0, run.stderr
            job = plistlib.loads((work / 'job.plist').read_bytes())
            assert job['ProgramArguments'] == ['/bin/bash', str(work / 'run-once.sh')]
            assert (work / 'restarts').read_text() == 'restart\n'
            assert (work / 'config.plist').read_text() == 'trial'
        else:
            assert run.returncode != 0, mode
            assert not (work / 'restarts').exists(), mode
            assert (work / 'restored').exists(), (mode, run.stderr)
            assert (work / 'config.plist').read_text() == 'stable'
            assert not (work / 'job.plist').exists(), mode
        results[mode] = 'pass'

# Actual unmount helper must leave a busy mounted EFI intact and return success.
helper = str(ROOT / 'trial-aux-h-20261108/esp.sh')
run = subprocess.run(['/bin/bash'], input='''set -euo pipefail
. "$TEST_HELPER"
diskutil() { return 1; }
sync() { :; }
esp_owned=1
esp_part=mock-disk
esp_mp=/path/that/must/not/be/removed
unmount_esp
test "$esp_owned" = 0
''', text=True, capture_output=True, env=dict(os.environ, TEST_HELPER=helper))
assert run.returncode == 0, run.stderr
results['busy_efi'] = 'pass'

for p in (ROOT / 'trial-aux-h-20261108').glob('*.sh'):
    run = subprocess.run(['/bin/bash', '-n', str(p)], capture_output=True, text=True)
    assert run.returncode == 0 and not run.stderr, (p.name, run.stderr)
results['all_shell_syntax_no_warnings'] = 'pass'
(ROOT / 'validation/aux-h-trial-script-tests.json').write_text(json.dumps(results, indent=2) + '\n')
print(json.dumps(results))
