#!/bin/bash
# BCn decoding on the CPU with helper threads (LGP_WEBGPU_DECODE_THREADS, #215)
# on the iPad, which has no BC in hardware: ABAB 0/2/3 with host clock and a
# fresh copy of one fixed save per run, in its own --save folder below
# Documents/abab-saves (the player's saves in Documents/saves stay untouched).
# Measured like on the Apple TV in #215: the --time-stats window with the most
# texture upload time (scene change), its uploads and fps, plus the whole runs.
#
#   tools/apple/measure_ipad_decode_threads.sh DEVICE SAVE_DIR [PRESS_FILE]
#
# Only while nobody plays: every run first checks that lgp_game is not running
# on the iPad and stops otherwise (IDLE_CHECK); it never ends a running game.
# The installed app must contain #215: with 2 or 3 helpers notes.txt shows the
# line "BCn decoding on the GPU thread and N helper(s)"; without it the
# setting did not arrive. Defaults: ORDER "0 2 3 3 2 0", WARMUP 2; every other
# setting and the output as in measure_device_abab.sh.
export VAR=LGP_WEBGPU_DECODE_THREADS PLATFORM=ios TAG=dt
export ORDER=${ORDER:-0 2 3 3 2 0} WARMUP=${WARMUP-2}
exec "$(dirname "$0")/measure_device_abab.sh" "$@"
