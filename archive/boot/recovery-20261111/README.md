# Native boot recovery — 2026-11-11

Source snapshot: local adaptation commit `a2bf4d4270595f49925858508a84afd42a33749f`; implementation introduced in `40ce0f6`. Source-to-public file hashes and substitutions are recorded in [provenance](../../../evidence/adaptation-a2bf4d4.json).

This revision addresses a persistence failure: native startup wrote a safe EFI profile and stock AuxKC early, but an interruption before the final healthy commit could leave subsequent boots in basic display indefinitely. Accepted GPU, Wi-Fi, Bluetooth and sensor binaries are unchanged.

## Recovery policy

- Persist the native attempt and its stage before risky work.
- Replenish one automatic recovery credit only after strict fresh health and a successful next-boot commit. Reserve it before preparing native cache/gates; write EFI last.
- A safe boot may prepare the next user-operated restart. It never restarts the machine automatically. A failed retry pauses; interrupted preparation resumes at most once.
- Respect an intentional safe boot without a recorded failure. Provide visible status and an explicit manual retry.
- Skip a WindowServer replacement only when strict current native/HiDPI health already passes. Otherwise record start, five-second heartbeats and terminal exit.
- Use boot-only rollback on commit failure, retaining accepted GPUBundles.

## Fresh display acceptance

`LiveDisplayAudit.m` queries the GUI session and optionally selects the target mode. `probe-display.sh` requests a new nonce and validates the same boot UUID, generation time, selection result and live geometry through `validate-display.sh`. Copying an old cache file cannot satisfy this protocol.

Acceptance requires physical 3840×2160 at approximately 60 Hz, logical 1920×1080 and HiDPI=true, together with native driver, Metal and scanout checks. The tested OpenCore profiles retain `RebaseRegions=true`, a five-second picker timeout and Tahoe as the default.

## Snapshot layout and reuse

`payload/` preserves the original flat staging layout. On the test machine, manager/state/check/notification files and their manifests reside under `BootPolicy.1`; startup, health, commit, display-probe and ESP helpers reside in its parent graphics package. `display-audit/` contains the GUI query source. `tests/` contains the isolated state-machine test.

These are reference sources, not an installer. Account `hackintosh`, example ESP/Data UUIDs ending in `001`/`002`, fixed package paths, UID assumptions, profile hashes and artifact manifests must be reviewed for the target machine. Manifests describe external artifacts, which are not bundled here. Private deployment installers, complete EFI profiles, caches, compiled apps, backup archives and raw logs are omitted. Historical scripts under `archive/graphics/scripts/` remain historical; this directory records the newer boot workflow.

Run the state test on macOS as an ordinary user:

```sh
bash archive/boot/recovery-20261111/tests/state-tests.sh
```

It writes only to a disposable temporary directory. Do not execute the hardware-facing payload scripts to run this test.

## Evidence and limits

The [runtime summary](../../../evidence/native-boot-20261111.json) records one successful user-operated reboot: fresh 4K60/2× HiDPI, stage 7/accel 2, scanout flips 812→814 in two seconds, a successful native next-boot commit and healthy recovery state with one credit. All 56 protected artifacts matched after boot. The switch helper ran and exited successfully; its conditional skip was not exercised.

Development checks covered seven state scenarios, the extracted arm transaction under mocked success/audit/EFI failures, and mocked switch success/failure/timeout/PID cases. Only the isolated state test is self-contained in this publication; private transaction fixtures are omitted. This verifies recovery and one healthy boot, not elimination of the original intermittent desktop-transition interruption or long-term stability.
