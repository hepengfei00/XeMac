#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
file=$1 nonce=$2 boot=$3 earliest=$4
[ -f "$file" ] && [ ! -L "$file" ]
value() { plutil -extract "$1" raw -o - "$file"; }
[ "$(value schema)" = 1 ]
[ "$(value nonce)" = "$nonce" ]
[ "$(value boot_uuid)" = "$boot" ]
[ "$(value generated_unix)" -ge "$earliest" ]
[ "$(value online)" = true ]
if [ "$(value selection_requested)" = true ]; then
    [ "$(value selection_candidate)" = true ]
    [ "$(value selection_rc)" = 0 ]
fi
for pair in 'pixel_width 3840' 'pixel_height 2160' 'logical_width 1920' 'logical_height 1080' 'hidpi true'; do
    read -r key wanted <<< "$pair"
    [ "$(value "current.$key")" = "$wanted" ]
done
value current.refresh_hz | awk '{if (!($1>59 && $1<61)) exit 1}'
echo FRESH_SAME_BOOT_4K60_HIDPI_CONFIRMED
