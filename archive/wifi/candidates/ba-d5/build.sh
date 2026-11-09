#!/bin/bash
set -euo pipefail
root=$(cd "$(dirname "$0")/source" && pwd)
cd "$root"
backend_c_sources='fw_blobs89.c rtw88_compat.c rtw88_firmware.c rtw89_compat.c Senmiko.c acpi.c cam.c chan.c coex.c core.c debug.c efuse.c efuse_be.c fw.c mac.c mac80211.c mac_be.c pci.c pci_be.c phy.c phy_be.c ps.c regd.c rtw8852a.c rtw8852a_rfk.c rtw8852a_rfk_table.c rtw8852a_table.c rtw8852ae.c sar.c ser.c util.c wow.c'
xcodebuild -quiet -project "$root/AirportRTW.xcodeproj" -target AirportRTW-Tahoe \
 -configuration Release ARCHS=x86_64 CODE_SIGNING_ALLOWED=NO MODULE_VERSION=2.3.1d5 \
 EXCLUDED_SOURCE_FILE_NAMES="$backend_c_sources" \
 'OTHER_LDFLAGS=$(inherited) $(DERIVED_FILE_DIR)/librtw89_c.a $(DERIVED_FILE_DIR)/librtw88_c.a $(DERIVED_FILE_DIR)/librtw_sae.a' \
 SYMROOT="$root/build/native-fix-release" OBJROOT="$root/build/native-fix-objects"
