import os, sys, stat, json, plistlib, subprocess, hashlib, shutil
from pathlib import Path

assert os.geteuid() == 0
os.environ['LC_ALL'] = 'C'
stage = Path(__file__).resolve().parent
payload = stage / 'payload'
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
def run(*args):
    return subprocess.check_output(args, stderr=subprocess.STDOUT)
def info(dev):
    return plistlib.loads(run('/usr/sbin/diskutil', 'info', '-plist', dev))
e = info('00000000-0000-0000-0000-000000000001')
d = info('00000000-0000-0000-0000-000000000002')
assert not e['Internal'] and e['BusProtocol'] == 'USB'
assert info(d['APFSPhysicalStores'][0]['APFSPhysicalStore'])['ParentWholeDisk'] == e['ParentWholeDisk']
assert info(e['ParentWholeDisk'])['TotalSize'] == EXAMPLE_SYSTEM_DISK_BYTES
data = Path(d['MountPoint'])
assert str(data).startswith('/Volumes/')
root = data / 'Library/Application Support/WTLArrowTiming.EXAMPLE'
pkg = data / 'Library/Application Support/WTLStable/WTL-265K-Recovery-20261109-v3'
boot = root / 'BootstrapRebase.1'
assert not pkg.exists() and not boot.exists()
assert sha(root/'trial-config.plist') == 'b68bdd2a54fce7b54103757ba37ab74e4cfc8d2eb20b69d53c01bf4fe1a8380c'
assert sha(root/'stable-config.plist') == 'ec385e2c5194728766138ddf8c2a74c206299b61bc27ec5d3936ed76ed87abe6'
assert sha(root/'onboot-stable.sh') == 'a1b88da5927d10957d3c008aec0d6b5293769ad46fd38058627f0feba4686bdc'
assert sha(root/'restore.sh') == '20d1063c6bf1fe50b7afb69956023b755fc0718022aefc448a170253c03d25fe'
cache = data/'Library/KernelCollections/AuxiliaryKernelExtensions.kc'
assert sha(cache) == '22f90766efeba40dde77dbb09d24d06ca87661bf378b49a9afdcc80ae7cb1682'
native_cache = root/'Arrow-Native-candidate.kc'
assert sha(native_cache) == '910da863fd31d575d792fdd3f2bb2bf59c295010fe2cc2a584e6ceffaf52fa27'
for name, h in json.loads((stage/'payload-hashes.json').read_text()).items():
    assert sha(payload/name) == h, name
expected = {}
for line in (stage/'expected-artifacts.sha256').read_text().splitlines():
    h, name = line.split('  ', 1); expected[name] = h
asset_modes = json.loads((stage/'asset-modes.json').read_text())
for name, h in expected.items():
    target = data/name
    if name in asset_modes:
        assert not target.exists() and sha(stage/'assets'/name) == h, name
    elif target != cache:
        assert sha(target) == h, name
run('/usr/sbin/diskutil', 'mount', 'readOnly', e['DeviceIdentifier'])
esp = Path(info(e['DeviceIdentifier'])['MountPoint'])
config = esp/'EFI/OC/config.plist'
assert sha(config) == 'ec385e2c5194728766138ddf8c2a74c206299b61bc27ec5d3936ed76ed87abe6'
for line in (stage/'enabled-files.sha256').read_text().splitlines():
    h, name = line.split('  ', 1); assert sha(esp/'EFI/OC'/name) == h, name
pkg.mkdir(parents=True, mode=0o700)
backup = pkg/'backup'; backup.mkdir(mode=0o700)
shutil.copytree(esp/'EFI', backup/'EFI-before')
before_efi = {str(p.relative_to(esp/'EFI')):sha(p) for p in (esp/'EFI').rglob('*') if p.is_file()}
for n, h in before_efi.items(): assert sha(backup/'EFI-before'/n) == h
saved = {}
desktop = data/'Users/hackintosh/Desktop/准备原盘下次启动.command'
changed = [root/'stable-config.plist', root/'onboot-stable.sh', cache, config]
if desktop.is_file(): changed.append(desktop)
for i, target in enumerate(changed):
    saved[target] = (target.read_bytes(), stat.S_IMODE(target.stat().st_mode))
    (backup/f'original-{i}').write_bytes(saved[target][0])
for f in payload.iterdir():
    if f.is_file(): shutil.copyfile(f, pkg/f.name)
run('/usr/sbin/diskutil', 'unmount', e['DeviceIdentifier'])
run('/usr/sbin/diskutil', 'mount', e['DeviceIdentifier'])
assert Path(info(e['DeviceIdentifier'])['MountPoint']) == esp
def atomic(target, raw, mode=0o644):
    target.parent.mkdir(parents=True, exist_ok=True)
    temp = target.with_name(target.name+'.recovery-new')
    temp.write_bytes(raw); temp.chmod(mode); os.chown(temp, 0, 0)
    if target == cache and target.exists(): run('/usr/bin/chflags', 'norestricted', str(target))
    os.replace(temp, target)
    if target == cache: run('/usr/bin/chflags', 'restricted', str(target))
created = []
try:
    for name, mode in asset_modes.items():
        target = data/name
        atomic(target, (stage/'assets'/name).read_bytes(), mode); created.append(target)
    atomic(cache, native_cache.read_bytes(), 0o744)
    boot.mkdir(mode=0o700)
    atomic(boot/'consume.sh', (payload/'consume.sh').read_bytes(), 0o755)
    atomic(boot/'bootstrap.private.plist', (payload/'config.bootstrap.private.plist').read_bytes(), 0o600)
    atomic(boot/'PENDING', b'OFFLINE_RECOVERY_PENDING\n', 0o600)
    atomic(root/'stable-config.plist', (payload/'config.fallback.private.plist').read_bytes())
    atomic(root/'onboot-stable.sh', (payload/'onboot-stable.sh').read_bytes(), 0o755)
    atomic(config, (payload/'config.bootstrap.private.plist').read_bytes())
    if desktop in saved: desktop.unlink()
    for name, h in expected.items(): assert sha(data/name) == h, name
    for n, h in before_efi.items():
        if n != 'OC/config.plist': assert sha(esp/'EFI'/n) == h, n
    assert sha(config) == 'a275ba7a9bc1562cba89ec870de976de74f272660f413b973289d7c696e56d63'
    assert sha(root/'trial-config.plist') == 'b68bdd2a54fce7b54103757ba37ab74e4cfc8d2eb20b69d53c01bf4fe1a8380c'
    assert sha(root/'stable-config.plist') == '09409ec0484908eaf5f00d16d4538ee158bb41a633696483fbf13217c88ed923'
    result = {'installed':True, 'boot_pending':True, 'restored_assets':len(created), 'protected_artifacts':len(expected), 'other_efi_unchanged':len(before_efi)-1, 'native_profile_unchanged':True, 'rebase_all_profiles':True, 'manual_arm_entry_archived':desktop in saved, 'restarted':False}
    (pkg/'applied.json').write_text(json.dumps(result, indent=2))
    (stage/'applied.json').write_text(json.dumps(result, indent=2)); os.chown(stage/'applied.json', int(os.environ['SUDO_UID']), int(os.environ['SUDO_GID']))
except BaseException:
    for target, (raw, mode) in saved.items():
        try:
            if not target.exists() or sha(target) != hashlib.sha256(raw).hexdigest(): atomic(target, raw, mode)
        except BaseException as ex: print('RESTORE_FAILED',str(target),str(ex), flush=True)
    for target in reversed(created):
        try: target.unlink()
        except OSError: pass
    if boot.exists(): shutil.rmtree(boot)
    (pkg/'INSTALL_FAILED').write_text('Rolled back; no restart\n')
    raise
os.sync()
run('/usr/sbin/diskutil', 'unmount', e['DeviceIdentifier'])
run('/usr/sbin/diskutil', 'mount', 'readOnly', e['DeviceIdentifier'])
print('Setup complete。图形资源、缓存和首次启动准备已恢复；回退保留网络/蓝牙/温度。未重启。', flush=True)
