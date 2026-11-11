#!/bin/bash
set -euo pipefail
export LC_ALL=C
here=$(cd "$(dirname "$0")" && pwd)
. "$here/../payload/state-machine.sh"
work=$(mktemp -d /private/tmp/boot-policy-tests.XXXXXX)
trap 'rm -rf "$work"' EXIT
expect_rc() { local wanted=$1 rc=0; shift; "$@" || rc=$?; [ "$rc" = "$wanted" ] || { echo "unexpected rc=$rc want=$wanted for $*"; exit 1; }; }
check() { sm_read; [ "$SM_PHASE:$SM_CREDITS:$SM_RESUMES" = "$1" ]; }
STATE_FILE="$work/daily.plist"
sm_seed OLD
sm_reserve SAFE1; check arming:0:0
sm_armed SAFE1; expect_rc 2 sm_reserve SAFE1
sm_begin NATIVE1; sm_success NATIVE1; check healthy:1:0
sm_begin NATIVE2; sm_stage NATIVE2 windowserver_switch; sm_fail NATIVE2 BOOT_EXIT_143
sm_reserve SAFE2; sm_armed SAFE2; check armed:0:0
sm_begin NATIVE3; sm_fail NATIVE3 BOOT_EXIT_1
expect_rc 3 sm_reserve SAFE3; check paused:0:0
expect_rc 3 sm_reserve SAFE4; check paused:0:0
sm_manual SAFE4; sm_reserve SAFE4; sm_armed SAFE4; check armed:0:0
printf 'PASS bounded automatic retry, exhaustion, explicit manual retry\n'
STATE_FILE="$work/resume.plist"
sm_seed OLD; sm_reserve SAFE1
sm_reserve SAFE2; check arming:0:1
expect_rc 3 sm_reserve SAFE3; check paused:0:1
printf 'PASS interrupted arm resumes at most once\n'
STATE_FILE="$work/powercut.plist"
sm_seed OLD; sm_reserve SAFE1
sm_begin NATIVE1; sm_success NATIVE1; check healthy:1:0
printf 'PASS EFI written before armed receipt still accepts actual native boot\n'
expect_rc 3 sm_reserve SAFE2; check paused:1:0
printf 'PASS explicit safe boot does not silently override a healthy baseline\n'
STATE_FILE="$work/ownership.plist"
sm_seed OLD; sm_reserve SAFE1; sm_armed SAFE1; sm_begin NATIVE1
expect_rc 2 sm_begin NATIVE1
expect_rc 10 sm_success OTHER
expect_rc 2 sm_manual NATIVE1
check running:0:0
printf 'PASS duplicate jobs and wrong-boot success rejected\n'
STATE_FILE="$work/iofailure.plist"
sm_seed OLD
before=$(shasum -a 256 "$STATE_FILE")
mv() { return 1; }
expect_rc 10 sm_reserve SAFE1
unset -f mv
[ "$before" = "$(shasum -a 256 "$STATE_FILE")" ]
check failed:1:0
printf 'PASS failed state reservation never permits hardware preparation\n'
/usr/libexec/PlistBuddy -c 'Set :phase unknown' "$STATE_FILE"
expect_rc 10 sm_reserve SAFE2
printf 'PASS corrupt state rejected without resetting retry credit\n'
