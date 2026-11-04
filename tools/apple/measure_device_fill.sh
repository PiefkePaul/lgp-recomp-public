#!/bin/bash
# Fill of the main target only (LGP_WEBGPU_FILL_SCALE, #312,
# docs/analysis/41-fuellflaeche-hauptziel.md) on the iPad or the Apple TV:
# ABAB without the switch (A, "-": the variable passed empty) against
# FILL_SCALE=0.75 on the 1280x720 target (B), host clock, a fresh copy of one
# fixed save per run in its own --save folder. Targets fps and frame time of
# --time-stats (report.txt); the picture of B is wrong on purpose.
#
#   [PLATFORM=tvos] tools/apple/measure_device_fill.sh DEVICE SAVE_DIR [PRESS_FILE]
#
# Both variables travel as --env on the launch command, after --app, so they
# win over settings.ini and leave it as it is (the script stops if settings.ini's
# args already name LGP_WEBGPU_FILL_SCALE). In notes.txt every B run must show
# the start-up line naming LGP_WEBGPU_FILL_SCALE ("draws fill 0.75 of
# 1280x720"); without it the installed app lacks #312 or the setting did not
# arrive. A has no such line. Handheld (the app's default): with --docked in
# settings.ini the main target is not 1280x720 and B changes nothing.
#
# Defaults: PLATFORM ios (iPad; Apple TV with PLATFORM=tvos), ORDER
# "- 0.75 0.75 -", WARMUP "-" (fills the pipeline cache; B uses the same
# pipelines), FILL_TARGETS 1280x720 (further sizes comma-separated). Every
# other setting and the output as in measure_device_abab.sh; on the iPad only
# while nobody plays (IDLE_CHECK).
HERE=$(cd "$(dirname "$0")" && pwd)
grep -q LGP_WEBGPU_FILL_SCALE "$HERE/../../gpu/webgpu/src/webgpu_backend.cpp" ||
    { echo "this checkout lacks #312 (LGP_WEBGPU_FILL_SCALE)" >&2; exit 2; }
export VAR=LGP_WEBGPU_FILL_SCALE TAG=${TAG:-fill} PLATFORM=${PLATFORM:-ios}
export ORDER=${ORDER:-- 0.75 0.75 -} WARMUP=${WARMUP--}
export EXTRA="--env LGP_WEBGPU_FILL_TARGETS=${FILL_TARGETS:-1280x720}${EXTRA:+ $EXTRA}"
exec "$HERE/measure_device_abab.sh" "$@"
