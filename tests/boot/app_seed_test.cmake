# Test of the pipeline cache seed in the app package (boot.sdk_app_packaged_seed,
# LGP_APP_PIPELINE_SEED): a cache written by one run, placed as
# pipeline-seed.lgppipes next to the packaged program's game/, is imported by
# a run with an empty app folder, and the player's own cache file stays the
# one in the app folder.
#   cmake -DPROGRAM=<packaged program> -DGAME=<game dir> -P app_seed_test.cmake
# The app folders go to the system's temporary folder: a pipeline cache
# inside the source tree (where build/ usually is) is refused.
get_filename_component(_package "${PROGRAM}" DIRECTORY)
set(_tmp "$ENV{TMPDIR}")
if(_tmp STREQUAL "")
    set(_tmp "/tmp")
endif()
string(RANDOM LENGTH 8 _id)
set(WORK "${_tmp}/lgp-app-seed-${_id}")
set(_seed "${_package}/pipeline-seed.lgppipes")
file(REMOVE_RECURSE "${WORK}")
file(REMOVE "${_seed}")
function(run_app dir)
    file(MAKE_DIRECTORY "${dir}")
    file(WRITE "${dir}/settings.ini" "game = ${GAME}\nargs = --renderer webgpu --input none --audio none\n")
    set(ENV{LGP_APP_DIR} "${dir}")
    execute_process(COMMAND "${PROGRAM}" --app --frames 2 RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    message(STATUS "${out}")
    if(NOT rc EQUAL 0)
        file(REMOVE "${_seed}")
        file(REMOVE_RECURSE "${WORK}")
        message(FATAL_ERROR "run in ${dir} failed (${rc}): ${err}")
    endif()
    set(out "${out}" PARENT_SCOPE)
endfunction()

run_app("${WORK}/first")
if(NOT EXISTS "${WORK}/first/webgpu-pipelines.lgppipes")
    file(REMOVE_RECURSE "${WORK}")
    message(FATAL_ERROR "the first run wrote no pipeline cache")
endif()
file(COPY_FILE "${WORK}/first/webgpu-pipelines.lgppipes" "${_seed}")
run_app("${WORK}/second")
file(REMOVE "${_seed}")
file(REMOVE_RECURSE "${WORK}")
string(FIND "${out}" "pipeline cache: ${WORK}/second/webgpu-pipelines.lgppipes (" _own)
string(FIND "${out}" "records from seed ${_seed})" _found)
if(_own EQUAL -1 OR _found EQUAL -1)
    message(FATAL_ERROR "the packaged seed was not used")
endif()
