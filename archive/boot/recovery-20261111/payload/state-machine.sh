#!/bin/bash
# State transitions only; callers provide a protected STATE_FILE and serialize writes.
sm_write() {
    case "$SM_PHASE" in failed|arming|armed|running|healthy|paused) ;; *) return 10;; esac
    case "$SM_CREDITS:$SM_RESUMES" in 0:0|0:1|1:0|1:1) ;; *) return 10;; esac
    local value
    for value in "$SM_OWNER" "$SM_ATTEMPT" "$SM_GOOD" "$SM_REASON" "$SM_STAGE"; do
        [[ "$value" =~ ^[A-Za-z0-9_.:+-]+$ ]] || return 10
    done
    local tmp
    tmp=$(mktemp "${STATE_FILE}.XXXXXX") || return
    if ! cat > "$tmp" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>version</key><integer>1</integer>
<key>phase</key><string>$SM_PHASE</string>
<key>credits</key><integer>$SM_CREDITS</integer>
<key>resumes</key><integer>$SM_RESUMES</integer>
<key>owner_boot</key><string>$SM_OWNER</string>
<key>attempt_boot</key><string>$SM_ATTEMPT</string>
<key>last_good_boot</key><string>$SM_GOOD</string>
<key>reason</key><string>$SM_REASON</string>
<key>stage</key><string>$SM_STAGE</string>
<key>updated_unix</key><integer>$(date +%s)</integer>
</dict></plist>
EOF
    then rm -f "$tmp"; return 10; fi
    plutil -lint "$tmp" >/dev/null || { rm -f "$tmp"; return 10; }
    chmod 644 "$tmp" || return 10
    mv -f "$tmp" "$STATE_FILE" || return 10
    sync || return 10
}
sm_read() {
    [ -f "$STATE_FILE" ] && [ ! -L "$STATE_FILE" ] || return 10
    [ "$(plutil -extract version raw -o - "$STATE_FILE")" = 1 ] || return 10
    SM_PHASE=$(plutil -extract phase raw -o - "$STATE_FILE") || return 10
    SM_CREDITS=$(plutil -extract credits raw -o - "$STATE_FILE") || return 10
    SM_RESUMES=$(plutil -extract resumes raw -o - "$STATE_FILE") || return 10
    SM_OWNER=$(plutil -extract owner_boot raw -o - "$STATE_FILE") || return 10
    SM_ATTEMPT=$(plutil -extract attempt_boot raw -o - "$STATE_FILE") || return 10
    SM_GOOD=$(plutil -extract last_good_boot raw -o - "$STATE_FILE") || return 10
    SM_REASON=$(plutil -extract reason raw -o - "$STATE_FILE") || return 10
    SM_STAGE=$(plutil -extract stage raw -o - "$STATE_FILE") || return 10
    case "$SM_PHASE" in failed|arming|armed|running|healthy|paused) ;; *) return 10;; esac
    case "$SM_CREDITS:$SM_RESUMES" in 0:0|0:1|1:0|1:1) ;; *) return 10;; esac
}
sm_seed() {
    [ ! -e "$STATE_FILE" ] || return 10
    SM_PHASE=failed SM_CREDITS=1 SM_RESUMES=0 SM_OWNER=$1 SM_ATTEMPT=$1
    SM_GOOD=none SM_REASON=LEGACY_INTERRUPTED_BOOT SM_STAGE=seed
    sm_write
}
sm_pause() {
    SM_PHASE=paused SM_OWNER=$1 SM_REASON=$2
    sm_write
}
sm_reserve() {
    local boot=$1
    sm_read || return
    case "$SM_PHASE" in
        failed|running)
            if [ "$SM_PHASE" = running ] && [ "$SM_OWNER" = "$boot" ]; then return 2; fi
            if [ "$SM_CREDITS" = 0 ]; then sm_pause "$boot" RETRY_LIMIT_REACHED; return 3; fi
            SM_CREDITS=0 SM_RESUMES=0 SM_PHASE=arming SM_OWNER=$boot
            SM_REASON=ONE_RETRY_RESERVED SM_STAGE=prepare_next_boot
            sm_write
            ;;
        arming)
            [ "$SM_OWNER" != "$boot" ] || return 2
            if [ "$SM_RESUMES" = 1 ]; then sm_pause "$boot" ARM_RESUME_LIMIT_REACHED; return 3; fi
            SM_RESUMES=1 SM_OWNER=$boot SM_REASON=RESUME_SAME_RESERVATION
            sm_write
            ;;
        armed)
            [ "$SM_OWNER" != "$boot" ] || return 2
            sm_pause "$boot" ARMED_BUT_SAFE_BOOT_SELECTED
            return 3
            ;;
        healthy) sm_pause "$boot" SAFE_BOOT_WITHOUT_RECORDED_FAILURE; return 3;;
        paused) SM_OWNER=$boot; sm_write; return 3;;
    esac
}
sm_armed() {
    sm_read || return
    [ "$SM_PHASE" = arming ] && [ "$SM_OWNER" = "$1" ] || return 10
    SM_PHASE=armed SM_REASON=NEXT_BOOT_NATIVE_READY SM_STAGE=await_manual_restart
    sm_write
}
sm_begin() {
    sm_read || return
    [ "$SM_PHASE:$SM_OWNER" != "running:$1" ] || return 2
    case "$SM_PHASE" in paused|failed) return 10;; esac
    SM_PHASE=running SM_OWNER=$1 SM_ATTEMPT=$1 SM_REASON=NATIVE_BOOT_RUNNING SM_STAGE=start
    sm_write
}
sm_fail() {
    sm_read || return
    [ "$SM_OWNER" = "$1" ] || return 10
    case "$SM_PHASE" in
        running) SM_PHASE=failed;;
        arming) SM_PHASE=paused;;
        *) return 10;;
    esac
    SM_REASON=$2
    sm_write
}
sm_success() {
    sm_read || return
    [ "$SM_PHASE:$SM_OWNER" = "running:$1" ] || return 10
    SM_PHASE=healthy SM_CREDITS=1 SM_RESUMES=0 SM_GOOD=$1
    SM_REASON=FRESH_HEALTH_AND_COMMIT_PASSED SM_STAGE=complete
    sm_write
}
sm_stage() {
    sm_read || return
    [ "$SM_PHASE:$SM_OWNER" = "running:$1" ] || return 10
    SM_STAGE=$2
    sm_write
}
sm_manual() {
    sm_read || return
    [ "$SM_PHASE:$SM_OWNER" != "running:$1" ] || return 2
    SM_PHASE=failed SM_CREDITS=1 SM_RESUMES=0 SM_OWNER=$1
    SM_REASON=EXPLICIT_MANUAL_RETRY SM_STAGE=manual
    sm_write
}
