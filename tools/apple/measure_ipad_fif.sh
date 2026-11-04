#!/bin/bash
# Frames in flight on the iPad (LGP_WEBGPU_FRAMES_IN_FLIGHT): is there a spike
# of driver memory at the scene near frame 8400 as on the Apple TV, and what
# does a limit of 3 cost? Plan in docs/analysis/32-metal-speicher-je-pipeline.md,
# "Messplan iPad Pro M1": A = 16 (counts only, practically never waits; shows
# the natural depth), B = 3, virtual clock and the vpress script from an empty
# save, so every run reaches the same scene; footprint parts on.
#
#   tools/apple/measure_ipad_fif.sh DEVICE EMPTY_SAVE_DIR PRESS_FILE
#
#   EMPTY_SAVE_DIR  an empty local folder (the press script starts a new game);
#                   each run gets its own copy below Documents/abab-saves
#   PRESS_FILE      the input script with frame triggers (vpress-pikachu);
#                   frame triggers only hold under the virtual clock
#
# Only while nobody plays (IDLE_CHECK stops instead of ending a game). Same
# app for all runs, no rebuild in between. Defaults: ORDER "16 3 16 3",
# WARMUP 16 (fills the pipeline cache; the Metal shader cache cannot be
# cleared without deleting the saves), FRAMES 10500, PAUSE 300, report over
# frames 8000-9000. Under the virtual clock the fps compare only between A and
# B. Besides report.txt of measure_device_abab.sh, every run gets
# <label>.memory.txt from tools/analyze/kill_run_report.py (footprint curve,
# parts that rose most, available, metal allocated). Not yet checked: whether
# `devicectl device copy to` takes an empty folder (try FRAMES=600 ORDER=3
# WARMUP= first).
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
[ $# -eq 3 ] || { echo "usage: measure_ipad_fif.sh DEVICE EMPTY_SAVE_DIR PRESS_FILE" >&2; exit 2; }
[ -z "$(ls -A "$2" 2>/dev/null)" ] || { echo "save folder is not empty: $2" >&2; exit 2; }
export VAR=LGP_WEBGPU_FRAMES_IN_FLIGHT PLATFORM=ios TAG=ipad-fif
export ORDER=${ORDER:-16 3 16 3} WARMUP=${WARMUP-16}
export FRAMES=${FRAMES:-10500} PAUSE=${PAUSE:-300} RUN_TIMEOUT=${RUN_TIMEOUT:-2400}
export FROM_FRAME=${FROM_FRAME:-8000} TO_FRAME=${TO_FRAME:-9000}
# Later options win: --clock virtual replaces the script's --clock host.
export EXTRA="--clock virtual --env LGP_HOST_MEMORY_PARTS=1${EXTRA:+ $EXTRA}"
export OUT=${OUT:-${LGP_PRIVATE_DIR:?LGP_PRIVATE_DIR is not set}/ios/ipad-fif/$(date +%Y%m%d-%H%M%S)}
"$HERE/measure_device_abab.sh" "$@"
status=$?
for log in "$OUT"/*.log; do
    [ -f "$log" ] || continue
    python3 "$REPO/tools/analyze/kill_run_report.py" "$log" >"${log%.log}.memory.txt" 2>&1
done
echo "memory per run: $OUT/*.memory.txt"
exit $status
