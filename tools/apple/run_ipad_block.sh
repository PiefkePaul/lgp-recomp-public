#!/bin/bash
# The iPad measurement block in one go (P7): the existing wrappers one after
# the other, with cooling pauses, the player's saves checked before and after,
# and nothing started while lgp_game runs on the iPad (someone may be playing).
#
#   1. dt    measure_ipad_decode_threads.sh  BCn helper threads 0/2/3 (#215)
#   2. fif   measure_ipad_fif.sh             frames in flight 16 against 3
#   3. hbf   measure_ipad_host_block_file.sh guest memory on a file 0/1 (#213)
#   4. ui    tools/ios/uitest.sh of #193     XCUITest screenshot as proof
#   -  fill  measure_device_fill.sh          main target fill 1 against 0.75
#                                            (#312); only when STEPS names it
#
#   APP=<lgp_game.app> tools/apple/run_ipad_block.sh DEVICE SAVE_DIR EMPTY_SAVE_DIR VPRESS_FILE [PRESS_FILE]
#
#   APP             the lgp_game.app built from current main (with #215, #271
#                   and the pipeline seed of #272); installed as an update
#                   before step 1 (keeps Documents/) and again after step 4
#   SAVE_DIR        fixed save for dt, hbf and fill (copied per run, never written)
#   EMPTY_SAVE_DIR  empty folder for fif (the press script starts a new game)
#   VPRESS_FILE     frame-trigger input script for fif (virtual clock)
#   PRESS_FILE      time/event-trigger input script for dt, hbf and fill (optional)
#
# Settings (environment):
#   STEPS       which steps, in this order (default "dt fif hbf ui"; e.g.
#               STEPS=fill for the fill measurement alone, after #312)
#   COOL        seconds of cooling between steps (default 900; the wrappers
#               keep their own pauses between runs)
#   WAIT_IDLE   seconds to wait for lgp_game to end before a step (default
#               1800, polled every 60 s); after that the block stops
#   INSTALL     1: install APP before step 1 and after step 4 (default 1)
#   SAVE_CHECK  1: fetch the save slots before and after, compare their MD5s
#               (default 1); exit 4 and keep the copy from before on a change
#   SAVE_SLOTS  folders below Documents to check (default "saves saves-2
#               saves-3 saves-4 saves-5")
#   UI_REPO     checkout of #193 (claude/p7-xcuitest) with a build tree
#               configured with -DLGP_IOS_UI_TESTS=ON; without it step ui is
#               skipped with a note
#   UI_BUILD    that build tree (default $UI_REPO/build/ios)
#   UI_SCRIPT   steps for LgpUITests.m (default: start screen, two shots)
#   DRY_RUN     1: run the checks and print the plan; on the device it only
#               reads the process list (whether lgp_game runs)
#   OUT         output folder (default $LGP_PRIVATE_DIR/ios/block/<time>)
#
# Step ui builds and installs the app of #193 through xcodebuild test; that is
# why it runs last and why APP is installed again afterwards, so the iPad ends
# on the measured main build. Everything the block writes stays in OUT below
# $LGP_PRIVATE_DIR.
#
# lgp_game counts as running as long as it is in the process list, also when
# it sits suspended in the background (devicectl cannot tell the foreground
# apart). So the app must be closed on the iPad (swiped away) before the
# block, or the block waits WAIT_IDLE and stops. It never ends the app itself.
# Checked on the Apple TV (read only, 2026-10-15): `devicectl device copy
# from` takes a folder as --source and copies it recursively, hidden .lgp-*
# entries included; a missing source fails (error 7000) and creates nothing.
set -u

BUNDLE=de.piefkepaul.lgp.game
USAGE="usage: APP=<lgp_game.app> run_ipad_block.sh DEVICE SAVE_DIR EMPTY_SAVE_DIR VPRESS_FILE [PRESS_FILE]"
DEVICE=${1:?$USAGE}
SAVE_DIR=${2:?$USAGE}
EMPTY_SAVE_DIR=${3:?$USAGE}
VPRESS_FILE=${4:?$USAGE}
PRESS_FILE=${5:-}
APP=${APP:?$USAGE}
STEPS=${STEPS:-dt fif hbf ui}
COOL=${COOL:-900}
WAIT_IDLE=${WAIT_IDLE:-1800}
INSTALL=${INSTALL:-1}
SAVE_CHECK=${SAVE_CHECK:-1}
SAVE_SLOTS=${SAVE_SLOTS:-saves saves-2 saves-3 saves-4 saves-5}
UI_REPO=${UI_REPO:-}
UI_BUILD=${UI_BUILD:-${UI_REPO:+$UI_REPO/build/ios}}
UI_SCRIPT=${UI_SCRIPT:-wait:25;shot:start;wait:5;shot:start-later}
DRY_RUN=${DRY_RUN:-0}
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
: "${LGP_PRIVATE_DIR:?LGP_PRIVATE_DIR is not set (logs belong there)}"
OUT=${OUT:-$LGP_PRIVATE_DIR/ios/block/$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUT" || exit 2
LOG=$OUT/block.txt
say() { echo "$*" | tee -a "$LOG"; }
fail() { say "STOP: $*"; exit 3; }
dc() { xcrun devicectl "$@"; }

# --- checks before anything touches the device ---
say "block started $(date '+%Y-%m-%d %H:%M:%S %z'), steps: $STEPS"
[ -d "$APP" ] && [ -f "$APP/lgp_game" ] || fail "APP is not an lgp_game.app: $APP"
[ -d "$SAVE_DIR" ] || fail "no save folder: $SAVE_DIR"
[ -d "$EMPTY_SAVE_DIR" ] && [ -z "$(ls -A "$EMPTY_SAVE_DIR")" ] || fail "not an empty folder: $EMPTY_SAVE_DIR"
[ -f "$VPRESS_FILE" ] || fail "no press file: $VPRESS_FILE"
[ -z "$PRESS_FILE" ] || [ -f "$PRESS_FILE" ] || fail "no press file: $PRESS_FILE"
run_steps=""
for step in $STEPS; do
    case $step in dt|fif|hbf) ;; fill)
        grep -q LGP_WEBGPU_FILL_SCALE "$REPO/gpu/webgpu/src/webgpu_backend.cpp" ||
            fail "step fill: this checkout lacks #312 (LGP_WEBGPU_FILL_SCALE)" ;; ui)
        if [ -z "$UI_REPO" ]; then
            say "note: step ui skipped, UI_REPO (checkout of #193 with -DLGP_IOS_UI_TESTS=ON) is not set"
            continue
        fi
        [ -x "$UI_REPO/tools/ios/uitest.sh" ] || fail "step ui: UI_REPO has no tools/ios/uitest.sh (checkout of #193)"
        [ -d "$UI_BUILD/lgp_recomp.xcodeproj" ] || fail "step ui: no Xcode project in $UI_BUILD" ;;
    *) fail "unknown step $step (dt fif hbf ui fill)" ;; esac
    run_steps="$run_steps${run_steps:+ }$step"
done
STEPS=$run_steps
[ -n "$STEPS" ] || fail "no step left to run"
# The app must come from a tree that has #215 (decode threads), #271 (fast
# BCn decoder) and #272 (pipeline seed in the package). Only the repository
# can be checked; that APP was built from it is on whoever built it.
head=$(git -C "$REPO" rev-parse --short HEAD)
for c in 697065d 0ea1084 580067b; do
    git -C "$REPO" merge-base --is-ancestor "$c" HEAD 2>/dev/null || fail "repo $head lacks $c (#215/#271/#272)"
done
say "repo $head, app $APP: $(shasum -a 256 "$APP/lgp_game" | cut -d' ' -f1)"
seed=$APP/pipeline-seed.lgppipes
[ -f "$seed" ] || fail "no pipeline-seed.lgppipes in APP (built without LGP_PRIVATE_DIR?)"
if [ -f "$LGP_PRIVATE_DIR/pipeline-seed.lgppipes" ]; then
    a=$(md5 -q "$seed" 2>/dev/null || md5sum "$seed" | cut -d' ' -f1)
    b=$(md5 -q "$LGP_PRIVATE_DIR/pipeline-seed.lgppipes" 2>/dev/null ||
        md5sum "$LGP_PRIVATE_DIR/pipeline-seed.lgppipes" | cut -d' ' -f1)
    say "seed in APP $a, in LGP_PRIVATE_DIR $b"
    [ "$a" = "$b" ] || say "note: the packaged seed differs from LGP_PRIVATE_DIR's"
fi

# --- helpers ---
# 0 = lgp_game runs, 1 = it does not, 2 = cannot tell.
game_running() {
    local list
    list=$(dc device info processes --device "$DEVICE" 2>&1) || return 2
    grep -q 'lgp_game' <<< "$list"
}
# Waits until lgp_game does not run, at most WAIT_IDLE seconds; stops the
# block otherwise. Never ends a running game.
wait_idle() {
    local waited=0
    while :; do
        game_running
        case $? in
            1) return 0 ;;
            2) fail "cannot list the iPad's processes ($1)" ;;
        esac
        [ "$waited" -ge "$WAIT_IDLE" ] &&
            fail "lgp_game still in the process list after $WAIT_IDLE s ($1); close the app on the iPad (also in the background)"
        [ "$waited" = 0 ] && say "lgp_game runs on the iPad ($1), waiting up to $WAIT_IDLE s"
        sleep 60
        waited=$((waited + 60))
    done
}
# One line per file under DIR: md5 and relative path, sorted.
manifest() {
    (cd "$1" && find . -type f -print0 | sort -z | xargs -0 md5 -r 2>/dev/null ||
        find . -type f -print0 | sort -z | xargs -0 md5sum)
}
# Fetches every save slot (Documents/saves, saves-2 ...) into OUT/<name>/ and
# writes <name>.md5, one line per file. "saves" must come over (it is where a
# fresh app saves); a later slot that does not come over counts as absent, and
# the same must hold after the block.
fetch_saves() {
    rm -rf "${OUT:?}/$1"
    mkdir -p "$OUT/$1"
    : >"$OUT/$1.md5"
    local slot
    for slot in $SAVE_SLOTS; do
        if dc device copy from --device "$DEVICE" --domain-type appDataContainer --domain-identifier "$BUNDLE" \
                --source "Documents/$slot" --destination "$OUT/$1/$slot" >>"$OUT/$1.copy" 2>&1; then
            manifest "$OUT/$1/$slot" | sed "s|\./|$slot/|" >>"$OUT/$1.md5"
        else
            [ "$slot" = saves ] && fail "could not fetch Documents/saves, see $1.copy"
            echo "absent $slot" >>"$OUT/$1.md5"
        fi
    done
    say "saves $1: $(grep -vc '^absent' "$OUT/$1.md5") files ($(grep -c '^absent' "$OUT/$1.md5") slots absent)"
}
install_app() {
    wait_idle "install"
    dc device install app --device "$DEVICE" "$APP" >>"$OUT/install.txt" 2>&1 || fail "install failed, see install.txt"
    say "installed $APP"
}
cool() { say "cooling $COOL s"; sleep "$COOL"; }

local_before=$(manifest "$SAVE_DIR")
if [ "$DRY_RUN" = 1 ]; then
    game_running
    case $? in
        0) say "dry run: lgp_game is in the iPad's process list now (playing or in the background); the block would wait" ;;
        1) say "dry run: lgp_game does not run on the iPad now" ;;
        *) say "dry run: cannot list the iPad's processes" ;;
    esac
    say "dry run: checks passed; would install ($INSTALL), check saves ($SAVE_CHECK), run: $STEPS, cooling $COOL s"
    exit 0
fi

# --- the block ---
wait_idle "start"
[ "$SAVE_CHECK" = 1 ] && fetch_saves saves-before
[ "$INSTALL" = 1 ] && install_app
first=1
for step in $STEPS; do
    [ "$first" = 1 ] || cool
    first=0
    wait_idle "$step"
    say "== $step $(date '+%H:%M:%S')"
    status=0
    case $step in
        dt)  OUT=$OUT/dt APP=$APP "$HERE/measure_ipad_decode_threads.sh" "$DEVICE" "$SAVE_DIR" ${PRESS_FILE:+"$PRESS_FILE"} || status=$? ;;
        fif) OUT=$OUT/fif APP=$APP "$HERE/measure_ipad_fif.sh" "$DEVICE" "$EMPTY_SAVE_DIR" "$VPRESS_FILE" || status=$? ;;
        hbf) OUT=$OUT/hbf APP=$APP "$HERE/measure_ipad_host_block_file.sh" "$DEVICE" "$SAVE_DIR" ${PRESS_FILE:+"$PRESS_FILE"} || status=$? ;;
        fill) OUT=$OUT/fill APP=$APP PLATFORM=ios "$HERE/measure_device_fill.sh" "$DEVICE" "$SAVE_DIR" ${PRESS_FILE:+"$PRESS_FILE"} || status=$? ;;
        ui)  "$UI_REPO/tools/ios/uitest.sh" "$UI_BUILD" "$DEVICE" "$OUT/ui" "$UI_SCRIPT" "--app --menu on" || status=$?
             [ "$INSTALL" = 1 ] && install_app ;;
    esac
    say "   $step ended with $status"
    # 3 = a wrapper found lgp_game running (someone started playing): stop.
    [ "$status" = 3 ] && fail "$step stopped because lgp_game was running"
    [ "$(manifest "$SAVE_DIR")" = "$local_before" ] || fail "local SAVE_DIR changed during $step"
done
if [ "$SAVE_CHECK" = 1 ]; then
    fetch_saves saves-after
    if cmp -s "$OUT/saves-before.md5" "$OUT/saves-after.md5"; then
        say "Paul's saves unchanged"
    else
        say "WARNING: Paul's saves changed during the block:"
        diff "$OUT/saves-before.md5" "$OUT/saves-after.md5" | tee -a "$LOG"
        say "the copy from before is in $OUT/saves-before"
        exit 4
    fi
fi
say "block finished $(date '+%Y-%m-%d %H:%M:%S %z'); results in $OUT/{dt,fif,hbf,ui,fill}"
