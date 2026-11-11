#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
[ "$(id -u)" = 0 ]
pkg=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$pkg/.." && pwd)
[ "$root" = '/Library/Application Support/WTLArrowTiming.li8Pgu' ]
hash() { shasum -a 256 "$1" | awk '{print $1}'; }
guard_payload() {
    [ "$(sw_vers -buildVersion)" = 25G332 ]
    [ "$(diskutil info -plist /System/Volumes/Data | plutil -extract VolumeUUID raw -o - -)" = 00000000-0000-0000-0000-000000000002 ]
    case "$(hash "$root/trial-config.plist")" in
        59f3723a60603dba90003d4a49bc22d8b1b77e6237c153fe862de9e6bef73f62|90ccd3aba73b88faece997e5ac1444839ece789b71bd3ec7df9ffb534c3e354f) ;;
        *) return 1;;
    esac
    [ "$(hash "$root/stable-config.plist")" = fe5c0b54c6729a9d385c07c890a1d6e4203546aee6f58926ea90d3a32a43f7a7 ]
    [ "$(hash "$root/gates.plist")" = 613bb51c7832b5c5c0c532cdb7c2bebb32e8eed9f6a52cb0352c886f1bd4c6fb ]
    [ "$(hash "$root/gate-audit")" = c58d3322e99475eb0e35bf7bed6007710eb028cb586a9739a07852643c62fe20 ]
    [ "$(hash "$root/AuxiliaryKernelExtensions-before.kc")" = 22f90766efeba40dde77dbb09d24d06ca87661bf378b49a9afdcc80ae7cb1682 ]
    case "$(hash /Library/KernelCollections/AuxiliaryKernelExtensions.kc)" in
        22f90766efeba40dde77dbb09d24d06ca87661bf378b49a9afdcc80ae7cb1682|910da863fd31d575d792fdd3f2bb2bf59c295010fe2cc2a584e6ceffaf52fa27) ;;
        *) return 1;;
    esac
    [ "$(plutil -extract Misc.Boot.Timeout raw -o - "$root/trial-config.plist")" = 5 ]
    [ "$(plutil -extract ACPI.Quirks.RebaseRegions raw -o - "$root/trial-config.plist")" = true ]
    (cd / && shasum -c "$pkg/protected-artifacts.sha256") > "$pkg/payload-check-last.log" 2>&1
}
guard_payload
