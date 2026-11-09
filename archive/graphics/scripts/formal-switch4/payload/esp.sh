mount_esp() {
    esp_part=
    for dev in $(diskutil list physical | awk '$2 == "EFI" {print $NF}'); do
        if [ "$(diskutil info -plist "$dev" | plutil -extract DiskUUID raw -o - -)" = 00000000-0000-0000-0000-000000000001 ]; then
            esp_part=$dev
            break
        fi
    done
    test -n "$esp_part"
    esp_mp=$(diskutil info -plist "$esp_part" | plutil -extract MountPoint raw -o - - 2>/dev/null || true)
    esp_owned=0
    if [ -z "$esp_mp" ]; then
        esp_mp=$(mktemp -d /private/tmp/wtl-arrow-esp.XXXXXX)
        /sbin/mount_msdos "/dev/$esp_part" "$esp_mp"
        esp_owned=1
    fi
    esp_config="$esp_mp/EFI/OC/config.plist"
}
unmount_esp() {
    sync
    if [ "${esp_owned:-0}" = 1 ]; then
        if diskutil unmount "$esp_part"; then
            rmdir "$esp_mp"
        else
            echo EFI_SYNCED_LEFT_MOUNTED_BECAUSE_BUSY
        fi
        esp_owned=0
    fi
}
write_config() {
    cp "$1" "$esp_config.arrow-new"
    cmp -s "$1" "$esp_config.arrow-new"
    mv -f "$esp_config.arrow-new" "$esp_config"
    cmp -s "$1" "$esp_config"
    sync
}
write_collection() {
    local path=/Library/KernelCollections/AuxiliaryKernelExtensions.kc
    cp "$1" "$path.arrow-new"
    cmp -s "$1" "$path.arrow-new"
    chown root:wheel "$path.arrow-new"
    chmod 744 "$path.arrow-new"
    chflags norestricted "$path"
    mv -f "$path.arrow-new" "$path"
    cmp -s "$1" "$path"
    chflags restricted "$path"
    sync
}
