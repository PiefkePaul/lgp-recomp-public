#!/bin/bash
# ABAB runs of lgp_game on an iPad or Apple TV: one environment variable
# (VAR) at different values, runs in alternating order, host clock, each run
# on a fresh copy of one fixed save in its own --save folder (P7). Wrappers
# with the settings of one measurement: measure_tvos_fif.sh (frames in flight
# on the Apple TV), measure_ipad_decode_threads.sh (BCn helper threads on the
# iPad), measure_device_fill.sh (fill of the main target, iPad or Apple TV).
#
#   VAR=NAME ORDER="A B B A" tools/apple/measure_device_abab.sh DEVICE SAVE_DIR [PRESS_FILE]
#
#   DEVICE      devicectl device (UDID or name, `xcrun devicectl list devices`)
#   SAVE_DIR    local folder with the fixed save (as given to --save on the
#               desktop); it is copied to the device before every run, so every
#               run starts from the same state and the original is never written
#   PRESS_FILE  input script for --press (time or event triggers; frame numbers
#               drift under the host clock); without it the game gets no input
#
# Mac only, device awake and unlocked, lgp_game installed (the script does not
# install). Settings (environment):
#
#   VAR         the variable, passed as --env VAR=value (required)
#   ORDER       values in run order (required), e.g. "0 2 3 3 2 0"; "-" passes
#               VAR empty (--env VAR=, for switches that are off when empty)
#               and is labelled "off"
#   WARMUP      value of one run before the measured ones that is not counted,
#               fills the pipeline cache (empty = none)
#   TAG         label prefix of the runs (default: VAR without LGP_WEBGPU_)
#   PLATFORM    tvos (data in Library/Caches) or ios (data in Documents;
#               default tvos)
#   IDLE_CHECK  1: before every run, stop if lgp_game is already running on
#               the device (someone is playing; devicectl would end that game);
#               default 1 on ios, 0 on tvos
#   FRAMES      frames per run (default 9000)
#   TIME_STATS  frames per --time-stats window (default 100)
#   FROM_FRAME  first frame the report counts (default 1; set it past loading)
#   TO_FRAME    last frame the report counts (default: all)
#   PAUSE       seconds between runs, for the device's temperature (default 180)
#   RUN_TIMEOUT seconds before a run is given up (default 1200)
#   AUDIO       --audio value (default none, as the earlier script runs)
#   APP         local lgp_game.app that was installed; its SHA-256 goes into
#               the notes
#   EXTRA       further lgp_game options, word-split
#   OUT         output folder (default $LGP_PRIVATE_DIR/<platform>/<tag>/<time>)
#
# Writes per run <OUT>/<label>.log (the app's logs/lgp.log), <label>.console
# (devicectl's output); in notes.txt per run the deepest frames in flight, the
# log lines naming VAR and the --time-stats window with the most texture
# upload time (`gpu work ... texture uploads`) with its fps; at the end
# report.txt from tools/analyze/perf_run_report.py, one --group per value, and
# the upload peaks per value. On ios the app's logs/lgp.log and lgp.prev.log
# are first copied to <OUT>/before/ (every run rotates them). Only the
# program's numbers; the logs stay in $LGP_PRIVATE_DIR all the same, the saves
# stay on the device (<data>/abab-saves/...) and in SAVE_DIR.
#
# Checked on the Apple TV (dry run, 600 frames, 2026-10-14): `copy to` with a
# folder as --source creates --destination with the folder's contents
# (`.lgp-*` included) and the game writes there; `process launch --console`
# returns with exit 0 when the app ends. The log line "app: ... saves" still
# names the default folder; only that start-up line, not the folder in use.
# The options after --app win over settings.ini's `args =`, which still apply;
# settings.ini is fetched read-only into <OUT>/settings.ini for the notes, and
# the script stops if its args already name VAR (that would mix the arms).
# If a run's log has no "nvn: frames" line, the run ended early or was cut
# off and is flagged. Not yet checked: the process listing IDLE_CHECK reads
# (`devicectl device info processes`; if it fails, the run does not start).
set -u

BUNDLE=de.piefkepaul.lgp.game
USAGE="usage: VAR=NAME ORDER='A B B A' measure_device_abab.sh DEVICE SAVE_DIR [PRESS_FILE]"
DEVICE=${1:?$USAGE}
SAVE_DIR=${2:?$USAGE}
PRESS_FILE=${3:-}
VAR=${VAR:?$USAGE}
ORDER=${ORDER:?$USAGE}
WARMUP=${WARMUP-}
TAG=${TAG:-$(echo "${VAR#LGP_WEBGPU_}" | tr 'A-Z_' 'a-z-')}
PLATFORM=${PLATFORM:-tvos}
case "$PLATFORM" in
    tvos) BASE=Library/Caches; IDLE_CHECK=${IDLE_CHECK:-0} ;;
    ios) BASE=Documents; IDLE_CHECK=${IDLE_CHECK:-1} ;;
    *) echo "PLATFORM: tvos or ios" >&2; exit 2 ;;
esac
FRAMES=${FRAMES:-9000}
TIME_STATS=${TIME_STATS:-100}
FROM_FRAME=${FROM_FRAME:-1}
TO_FRAME=${TO_FRAME:-}
PAUSE=${PAUSE:-180}
RUN_TIMEOUT=${RUN_TIMEOUT:-1200}
AUDIO=${AUDIO:-none}
EXTRA=${EXTRA:-}
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
: "${LGP_PRIVATE_DIR:?LGP_PRIVATE_DIR is not set (logs belong there)}"
OUT=${OUT:-$LGP_PRIVATE_DIR/$PLATFORM/$TAG/$(date +%Y%m%d-%H%M%S)}

[ -d "$SAVE_DIR" ] || { echo "no save folder: $SAVE_DIR" >&2; exit 2; }
[ -z "$PRESS_FILE" ] || [ -f "$PRESS_FILE" ] || { echo "no press file: $PRESS_FILE" >&2; exit 2; }
mkdir -p "$OUT" || exit 2
NOTES=$OUT/notes.txt
{
    echo "started $(date '+%Y-%m-%d %H:%M:%S %z')"
    echo "repo $(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo '?')"
    [ -n "${APP:-}" ] && echo "app $APP: $(shasum -a 256 "$APP/lgp_game" 2>/dev/null | cut -d' ' -f1)"
    echo "$PLATFORM, $VAR, idle check $IDLE_CHECK"
    echo "order: ${WARMUP:+warm-up $WARMUP, }$ORDER; frames $FRAMES, time stats $TIME_STATS, pause $PAUSE s"
    echo "save $SAVE_DIR ($(find "$SAVE_DIR" -type f | wc -l | tr -d ' ') files)"
    [ -n "$PRESS_FILE" ] && echo "press $PRESS_FILE: $(shasum -a 256 "$PRESS_FILE" | cut -d' ' -f1)"
    echo "audio $AUDIO, extra: ${EXTRA:-none}"
} | tee "$NOTES"

dc() { xcrun devicectl "$@"; }
fetch() {  # fetch DEVICE_PATH LOCAL_FILE COPY_LOG
    dc device copy from --device "$DEVICE" --domain-type appDataContainer --domain-identifier "$BUNDLE" \
        --source "$1" --destination "$2" >>"$3" 2>&1
}
# Whether lgp_game runs on the device now; 2 if that cannot be told.
game_running() {
    local list
    list=$(dc device info processes --device "$DEVICE" 2>&1) || return 2
    grep -q 'lgp_game' <<< "$list"
}
# On iOS, devicectl ends a running game (and someone may be playing it).
terminate=(--terminate-existing)
[ "$PLATFORM" = ios ] && terminate=()

# settings.ini, read only: its args apply to every run besides ours.
if fetch "$BASE/settings.ini" "$OUT/settings.ini" "$OUT/settings.copy"; then
    echo "settings.ini args: $(grep -E '^[[:space:]]*args[[:space:]]*=' "$OUT/settings.ini" | head -1 | cut -d= -f2- | sed 's/^ *//')" \
        | tee -a "$NOTES"
    if grep -E '^[[:space:]]*args[[:space:]]*=' "$OUT/settings.ini" | grep -qF "$VAR"; then
        echo "settings.ini args name $VAR: every run would carry it, stopping" | tee -a "$NOTES"
        exit 2
    fi
else
    echo "no settings.ini on the device (or not fetched, see settings.copy)" | tee -a "$NOTES"
fi

if [ "$PLATFORM" = ios ]; then
    mkdir -p "$OUT/before"
    fetch Documents/logs/lgp.log "$OUT/before/lgp.log" "$OUT/before/copy.txt" ||
        echo "no earlier lgp.log on the device" | tee -a "$NOTES"
    fetch Documents/logs/lgp.prev.log "$OUT/before/lgp.prev.log" "$OUT/before/copy.txt" || true
fi

# The --time-stats window with the most texture upload time and its fps:
# "frames A-B: uploads X ms (Y MiB), Z fps".
upload_peak() {
    awk '
        /^time: frames [0-9]+-[0-9]+: / {
            split($3, r, "[-:]"); fps[r[1]] = $7
        }
        /^gpu work, frames [0-9]+-[0-9]+: / {
            split($4, r, "[-:]")
            if (match($0, /texture uploads [0-9.]+ ms \([0-9.]+ MiB/)) {
                split(substr($0, RSTART, RLENGTH), u, "[ (]+")
                if (u[3] + 0 > best) { best = u[3] + 0; mib = u[5]; a = r[1]; b = r[2] }
            }
        }
        END {
            if (a == "") print "none"
            else printf "frames %s-%s: uploads %.1f ms (%s MiB), %s fps\n", a, b, best, mib, (a in fps ? fps[a] : "?")
        }' "$1"
}

# One run: fresh save copy, launch, wait, fetch the log.
run() {
    local label=$1 value=$2
    [ "$value" = - ] && value=""
    local save="abab-saves/$TAG-$(basename "$OUT")/$label"  # new per measurement, never reused
    if [ "$IDLE_CHECK" = 1 ]; then
        game_running
        case $? in
            0) echo "== $label: lgp_game is running on the device (someone playing?), stopping" | tee -a "$NOTES"
               exit 3 ;;
            2) echo "== $label: cannot list the device's processes, stopping (IDLE_CHECK=1)" | tee -a "$NOTES"
               exit 3 ;;
        esac
    fi
    echo "== $label: $VAR=${value:-(empty)}, save $BASE/$save" | tee -a "$NOTES"
    if ! dc device copy to --device "$DEVICE" --domain-type appDataContainer --domain-identifier "$BUNDLE" \
            --source "$SAVE_DIR" --destination "$BASE/$save" >"$OUT/$label.copy" 2>&1; then
        echo "   save copy failed, see $label.copy" | tee -a "$NOTES"
        return 1
    fi
    local press=()
    [ -n "$PRESS_FILE" ] && press=(--press "$(cat "$PRESS_FILE")")
    # shellcheck disable=SC2086  # EXTRA is meant to be word-split
    dc device process launch --device "$DEVICE" ${terminate[@]+"${terminate[@]}"} --console "$BUNDLE" -- \
        --app --clock host --env "$VAR=$value" --save "$save" \
        --frames "$FRAMES" --time-stats "$TIME_STATS" --keyboard none --no-movie-audio --audio "$AUDIO" \
        --nvn-unimplemented continue ${press[@]+"${press[@]}"} $EXTRA >"$OUT/$label.console" 2>&1 &
    local pid=$! start=$SECONDS
    while kill -0 "$pid" 2>/dev/null; do
        if [ $((SECONDS - start)) -ge "$RUN_TIMEOUT" ]; then
            echo "   gave up after $RUN_TIMEOUT s" | tee -a "$NOTES"
            kill "$pid" 2>/dev/null
            break
        fi
        sleep 5
    done
    wait "$pid" 2>/dev/null
    echo "   ran $((SECONDS - start)) s" | tee -a "$NOTES"
    sleep 5  # the app closes its log after the console detaches
    if ! fetch "$BASE/logs/lgp.log" "$OUT/$label.log" "$OUT/$label.copy"; then
        echo "   log not fetched, see $label.copy" | tee -a "$NOTES"
        return 1
    fi
    grep -q "nvn: frames" "$OUT/$label.log" || echo "   no 'nvn: frames' line: ended early or cut off" | tee -a "$NOTES"
    # The deepest the frames stacked up (at most the limit; short or calm runs
    # may stay below it).
    local max
    max=$(grep -o 'frames in flight [0-9]* (max [0-9]*)' "$OUT/$label.log" | sed 's/.*max \([0-9]*\))/\1/' | sort -n | tail -1)
    echo "   frames in flight max ${max:-none}; last window: $(grep 'time: frames' "$OUT/$label.log" | tail -1)" \
        | tee -a "$NOTES"
    # Whether the setting arrived: the program's lines naming the variable.
    echo "   lines naming $VAR: $(grep -F "$VAR" "$OUT/$label.log" | grep -v 'app: arguments' | head -2 | tr '\n' ' ')" \
        | tee -a "$NOTES"
    echo "   upload peak: $(upload_peak "$OUT/$label.log")" | tee -a "$NOTES"
}

# Label of a value in file names and groups ("-" is "off").
lab() { if [ "$1" = - ]; then echo off; else echo "$1"; fi; }
if [ -n "$WARMUP" ]; then
    run warmup "$WARMUP"
    sleep "$PAUSE"
fi
done_runs=""  # "<value> <log>" per line, runs that fetched a log
i=0
for value in $ORDER; do
    i=$((i + 1))
    label="$TAG$(lab "$value")-$i"
    run "$label" "$value" && done_runs="$done_runs$value $OUT/$label.log
"
    [ "$i" -lt "$(echo $ORDER | wc -w)" ] && sleep "$PAUSE"
done
echo "finished $(date '+%Y-%m-%d %H:%M:%S %z')" | tee -a "$NOTES"
# One group per value, in the order the values first ran.
groups=()
for value in $(echo $ORDER | tr ' ' '\n' | awk '!seen[$0]++'); do
    logs=$(printf '%s' "$done_runs" | awk -v l="$value" '$1 == l { printf "%s%s", sep, $2; sep = "," }')
    [ -n "$logs" ] && groups+=(--group "$TAG$(lab "$value")=$logs")
done
range=(--from "$FROM_FRAME")
[ -n "$TO_FRAME" ] && range+=(--to "$TO_FRAME")
[ "${#groups[@]}" -gt 0 ] && python3 "$REPO/tools/analyze/perf_run_report.py" ${groups[@]+"${groups[@]}"} "${range[@]}" \
    | tee "$OUT/report.txt"
{
    echo
    echo "upload peaks (the --time-stats window with the most texture upload time, per run):"
    printf '%s' "$done_runs" | while read -r value log; do
        echo "  $TAG$(lab "$value") $(basename "$log" .log): $(upload_peak "$log")"
    done
} | tee -a "$OUT/report.txt"
