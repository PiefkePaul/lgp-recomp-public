#!/bin/bash
# What the tvOS input did in a hand test (P7.4, #274,
# docs/analysis/35-tvos-eingabe.md): fetches the app's log from the Apple TV
# and counts how far the presses got. Read only on the device: it copies
# Library/Caches/logs/lgp.log (and lgp.prev.log) and changes nothing.
#
#   tools/apple/tvos_input_report.sh DEVICE [LOG]
#
#   DEVICE  the Apple TV (devicectl UDID or name)
#   LOG     a local lgp.log instead of fetching one (then DEVICE is unused)
#
# The game must have run with --input-log, e.g. `args = --input-log` in the
# app's settings.ini or `devicectl device process launch … -- --app
# --input-log`. Lines it counts:
#   "lgp: gamepad … is a TV remote: it joins slot 0"  SDL opened the remote
#   "lgp: gamepad … -> slot N"                         a controller and its slot
#   "lgp: input 1s pad … button N"                     SDL got N button events
#   "hid: 1s: … button presses: npad…=N (0x…)"         the game got N presses
# The SDL lines reach lgp.log through sdl_main.cpp (SDL_Log also to stderr).
#
# XCUIRemote (XCUITest) cannot stand in for the hand test: on the Apple TV 4K
# (tvOS 27.2, 2026-10-15) its presses reached neither the GameController
# framework nor SDL (0 events in 81 one-second windows), so only a person
# with the remote or a controller can check the input.
#
# Output in OUT (default $LGP_PRIVATE_DIR/tvos/input-report/<time>): the logs
# and verdict.txt.
set -u

BUNDLE=de.piefkepaul.lgp.game
USAGE="usage: tvos_input_report.sh DEVICE [LOG]"
DEVICE=${1:?$USAGE}
LOG_IN=${2:-}
: "${LGP_PRIVATE_DIR:?LGP_PRIVATE_DIR is not set (logs belong there)}"
OUT=${OUT:-$LGP_PRIVATE_DIR/tvos/input-report/$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUT" || exit 2

if [ -n "$LOG_IN" ]; then
    cp "$LOG_IN" "$OUT/lgp.log" || exit 2
else
    for f in lgp.log lgp.prev.log; do
        xcrun devicectl device copy from --device "$DEVICE" --domain-type appDataContainer \
            --domain-identifier "$BUNDLE" --source "Library/Caches/logs/$f" --destination "$OUT/$f" \
            >>"$OUT/copy.txt" 2>&1 || echo "$f not fetched (see copy.txt)"
    done
fi
[ -f "$OUT/lgp.log" ] || { echo "no lgp.log" >&2; exit 3; }
log=$OUT/lgp.log

grep -q 'lgp: input 1s pad\|hid: 1s:' "$log" ||
    echo "note: no --input-log lines in lgp.log; did the game run with --input-log?" | tee "$OUT/verdict.txt"
sdl=$(grep -o 'lgp: input 1s pad [0-9]* .*button [0-9]*' "$log" | sed 's/.*button //' | awk '{s += $1} END {print s + 0}')
hid=$(grep -o 'button presses:.*' "$log" | grep -o 'npad[0-9]*=[0-9]*' | cut -d= -f2 | awk '{s += $1} END {print s + 0}')
{
    echo "pads SDL opened:"
    grep -E 'lgp: gamepad [0-9]+ ' "$log" | sed 's/^/  /' | head -8
    echo "SDL button events (sum of 'input 1s' lines): $sdl"
    echo "per pad: $(grep -o 'lgp: input 1s pad [0-9]* "[^"]*": .*button [0-9]*' "$log" |
        sed 's/lgp: input 1s pad \([0-9]*\) \("[^"]*"\).*button \([0-9]*\)/\1 \2 \3/' |
        awk '{n = $NF; $NF = ""; s[$0] += n} END {for (k in s) printf "%s= %d; ", k, s[k]}')"
    echo "game button presses (sum of 'hid: 1s' lines): $hid"
    echo "buttons the game saw per npad: $(grep -o 'button presses:.*' "$log" | grep -o 'npad[0-9]*=[0-9]* (0x[0-9a-f]*)' |
        awk '{split($1, a, "="); bits[a[1]] = bits[a[1]] " " $2} END {for (k in bits) printf "%s%s; ", k, bits[k]}')"
    echo "sticks the game saw: $(grep -o 'stick max/held min: npad[^,]*' "$log" | sort | uniq -c | sort -rn | head -3 | tr -s ' ' | tr '\n' ';')"
    if [ "$hid" -gt 0 ]; then
        echo "VERDICT: the game got button presses"
    elif [ "$sdl" -gt 0 ]; then
        echo "VERDICT: SDL got button events, the game none (slot or style?)"
    else
        echo "VERDICT: no button reached SDL"
    fi
} | tee -a "$OUT/verdict.txt"
echo "logs and verdict in $OUT"
