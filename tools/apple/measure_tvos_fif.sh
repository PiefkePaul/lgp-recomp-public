#!/bin/bash
# fps of lgp_game on the Apple TV with different frames-in-flight limits
# (LGP_WEBGPU_FRAMES_IN_FLIGHT), ABAB with host clock and a fresh copy of one
# fixed save per run (P7.4, docs/analysis/28-tvos-machbarkeit.md). Only with
# HDMI connected: without it tvOS renders at 1080p and the fps do not compare.
#
#   tools/apple/measure_tvos_fif.sh DEVICE SAVE_DIR [PRESS_FILE]
#
# Defaults: ORDER "2 3 4 4 3 2" (0 = no limit), WARMUP 3; every other setting
# and the output as in measure_device_abab.sh.
export VAR=LGP_WEBGPU_FRAMES_IN_FLIGHT PLATFORM=tvos TAG=fif
export ORDER=${ORDER:-2 3 4 4 3 2} WARMUP=${WARMUP-3}
export OUT=${OUT:-${LGP_PRIVATE_DIR:?LGP_PRIVATE_DIR is not set}/tvos/fif-fps/$(date +%Y%m%d-%H%M%S)}
exec "$(dirname "$0")/measure_device_abab.sh" "$@"
