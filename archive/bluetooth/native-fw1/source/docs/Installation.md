# Installation

## Requirements

- An OpenCore setup.
- [Lilu](https://github.com/acidanthera/Lilu)
- [BlueToolFixup](https://github.com/OpenIntelWireless/BrcmPatchRAM) — required on
  macOS 12 (Monterey) and later to enable third-party Bluetooth.
- A supported controller — see [Supported Devices](Supported-Devices.md).

No injector kext is needed.

## Building

```sh
make            # -> build/RealtekBluetoothFirmware.kext
make test       # host-side unit test of the firmware parser
make fw         # regenerate include/FwBinary.cpp from fw/*.bin
make clean
```

The result is a standard kext bundle ready to be code-signed and injected.

## Installing with OpenCore

1. Copy `RealtekBluetoothFirmware.kext` and `BlueToolFixup.kext` into
   `EFI/OC/Kexts`.
2. Add both to `config.plist` under `Kernel > Add`.

## Verifying

After reboot, Bluetooth should appear in System Settings. To confirm the
firmware upload ran:

```sh
log show --last boot --predicate 'eventMessage CONTAINS "RealtekFirmware"'
```

For example, on an RTL8822CE you should see the matched chip, `rom_version 3`, the parsed
payload size, the per-fragment download, and `firmware download complete`.

## Troubleshooting

- **No Bluetooth after a clean upload** (the log shows `firmware download
  complete` but Bluetooth never appears): almost always a Lilu/BlueToolFixup
  version that doesn't match your macOS build. Update those kexts.
- **Device not matched at all** (no `RealtekFirmware` log lines): your USB
  product ID isn't in `Info.plist` — see [Supported Devices](Supported-Devices.md).
