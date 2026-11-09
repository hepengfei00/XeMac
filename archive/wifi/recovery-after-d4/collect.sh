#!/bin/bash
set -u
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
printf 'BOOT_UUID='; sysctl -n kern.bootsessionuuid
printf 'OS_BUILD='; sw_vers -buildVersion
printf 'CONSOLE='; stat -f '%Su' /dev/console
echo LOADED_WIRELESS_MODULES
kmutil showloaded --list-only 2>/dev/null | grep -Ei 'AirportRTW|RTWBackend|RTWEthernet|itlwm|AirportItlwm' || true
echo WIRELESS_SERVICE_TREE
ioreg -p IOService -w0 | grep -Ei 'AirportRTW|RTWCore89|AirportRTWSkywalk|RTWBackendDiag|RTWEthernet' || true
echo DRIVER_SCALAR_STATE
ioreg -r -c AirportRTW -l -d 1 -w0 | grep -E '"(RTWLocalPowerStage|RTWLocalPowerResult|RTWNativeRxBuild|RTWNativeRxMode|RTWBAProbeBuild|RTWStartupDiagnostics|RTWStartupFailed|IOBSDName)"' || true
echo WIRELESS_NETWORK_INTERFACE
ifconfig en2 2>&1 | grep -E '^en2:|inet |status:|does not exist' || true
echo ETHERNET_NETWORK_INTERFACE
ifconfig en3 2>&1 | grep -E '^en3:|inet |status:|does not exist' || true
echo GRAPHICS_HEALTH
root='/Library/Application Support/WTLArrowTiming.EXAMPLE'
bash "$root/health.sh" 2>&1 || true
shasum -a 256 "$root/trial-config.plist" "$root/restore-efi-only.sh" /Library/KernelCollections/AuxiliaryKernelExtensions.kc 2>&1 || true
sysctl debug.xegfx_stage debug.xegfxaccel_boot debug.xegfx_agdc_k5_why 2>&1 || true
echo READ_ONLY_RECOVERY_CHECK_COMPLETE
