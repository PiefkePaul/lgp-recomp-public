#!/bin/bash
# Guest memory backed by a file on the iPad (LGP_HOST_BLOCK_FILE, #213): does
# it lower the footprint jetsam counts (2505 MiB at frame 3000 anonymous, 1333
# MiB on the Apple TV with the file) and what does it cost in fps? The variable
# works at run time on any build (POSIX); #213 only adds the build option for
# the default, so one app serves both arms. ABAB 0/1, host clock, a fresh copy
# of one fixed save per run (fps compare only under the host clock).
#
#   tools/apple/measure_ipad_host_block_file.sh DEVICE SAVE_DIR [PRESS_FILE]
#
#   PRESS_FILE  with time or event triggers (frame numbers drift under the
#               host clock)
#
# Only while nobody plays (IDLE_CHECK). Defaults: ORDER "0 1 1 0", WARMUP 0,
# FRAMES 9000, footprint parts on; everything else as in
# measure_device_abab.sh. notes.txt gets per run whether the file backing was
# on (log line "host block: guest memory backed by a file"), and every run
# <label>.memory.txt from tools/analyze/kill_run_report.py. Write load on the
# flash under memory pressure is not measured here.
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
export VAR=LGP_HOST_BLOCK_FILE PLATFORM=ios TAG=hbf
export ORDER=${ORDER:-0 1 1 0} WARMUP=${WARMUP-0}
export EXTRA="--env LGP_HOST_MEMORY_PARTS=1${EXTRA:+ $EXTRA}"
export OUT=${OUT:-${LGP_PRIVATE_DIR:?LGP_PRIVATE_DIR is not set}/ios/host-block-file/$(date +%Y%m%d-%H%M%S)}
"$HERE/measure_device_abab.sh" "$@"
status=$?
for log in "$OUT"/*.log; do
    [ -f "$log" ] || continue
    if grep -q "host block: guest memory backed by a file" "$log"; then backed=file; else backed=anonymous; fi
    echo "$(basename "$log" .log): guest memory $backed" | tee -a "$OUT/notes.txt"
    python3 "$REPO/tools/analyze/kill_run_report.py" "$log" >"${log%.log}.memory.txt" 2>&1
done
echo "memory per run: $OUT/*.memory.txt"
exit $status
