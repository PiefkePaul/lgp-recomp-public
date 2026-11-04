# P4 shader spike: Maxwell decoder, IR and WGSL backend. Own test shaders only
# (tests/gpu/shaders, assembled with envyas); game shaders never enter the
# repository or CI.

# Decoder vs. envydis on generated vectors (tools/shader/gen_maxwell_vectors.py).
add_executable(lgp_test_maxwell_decoder test_maxwell_decoder.cpp)
target_link_libraries(lgp_test_maxwell_decoder PRIVATE lgp_gpu_shader)
if(EMSCRIPTEN)
    # Node needs direct file system access to read the vectors file.
    target_link_options(lgp_test_maxwell_decoder PRIVATE -sNODERAWFS=1)
endif()
add_test(NAME gpu.maxwell_decoder
    COMMAND lgp_test_maxwell_decoder "${CMAKE_CURRENT_SOURCE_DIR}/maxwell_vectors.txt")
set_tests_properties(gpu.maxwell_decoder PROPERTIES LABELS "gpu")

# ASTC decoder vs. ARM's astcenc on generated vectors (tools/texture/gen_astc_vectors.py).
add_executable(lgp_test_astc texture/test_astc.cpp)
target_link_libraries(lgp_test_astc PRIVATE lgp_gpu_texture)
if(EMSCRIPTEN)
    target_link_options(lgp_test_astc PRIVATE -sNODERAWFS=1)
endif()
add_test(NAME gpu.astc COMMAND lgp_test_astc "${CMAKE_CURRENT_SOURCE_DIR}/texture/astc_vectors.txt")
set_tests_properties(gpu.astc PROPERTIES LABELS "gpu;texture")

# BCn decoder (P7.3) vs. Pillow's decoder on generated vectors (tools/texture/gen_bc_vectors.py).
add_executable(lgp_test_bc texture/test_bc.cpp)
target_link_libraries(lgp_test_bc PRIVATE lgp_gpu_texture)
if(EMSCRIPTEN)
    target_link_options(lgp_test_bc PRIVATE -sNODERAWFS=1)
endif()
add_test(NAME gpu.bc COMMAND lgp_test_bc "${CMAKE_CURRENT_SOURCE_DIR}/texture/bc_vectors.txt")
set_tests_properties(gpu.bc PROPERTIES LABELS "gpu;texture")

# The remaining tests drive the maxwell2wgsl executable from Python; under
# Emscripten that would be a Node script, so they run on native builds only.
if(EMSCRIPTEN)
    return()
endif()

set(LGP_WGSL_OUT "${CMAKE_CURRENT_BINARY_DIR}/wgsl")
set(_checks "${CMAKE_CURRENT_SOURCE_DIR}/wgsl_checks.py")

add_test(NAME gpu.shader_disasm
    COMMAND "${Python3_EXECUTABLE}" "${_checks}" disasm $<TARGET_FILE:maxwell2wgsl>)
set_tests_properties(gpu.shader_disasm PROPERTIES LABELS "gpu")

add_test(NAME gpu.translate
    COMMAND "${Python3_EXECUTABLE}" "${_checks}" translate $<TARGET_FILE:maxwell2wgsl> "${LGP_WGSL_OUT}")
set_tests_properties(gpu.translate PROPERTIES LABELS "gpu" FIXTURES_SETUP lgp_wgsl)

# Constructs the translator must refuse (e.g. MEMBAR without a following BAR/EXIT).
add_test(NAME gpu.reject
    COMMAND "${Python3_EXECUTABLE}" "${_checks}" reject $<TARGET_FILE:maxwell2wgsl> "${CMAKE_CURRENT_BINARY_DIR}/reject")
set_tests_properties(gpu.reject PROPERTIES LABELS "gpu")

# WGSL validation with naga (wgpu/Firefox front end); skipped if naga is absent.
add_test(NAME gpu.naga_validate COMMAND "${Python3_EXECUTABLE}" "${_checks}" naga "${LGP_WGSL_OUT}")
set_tests_properties(gpu.naga_validate PROPERTIES
    LABELS "gpu" FIXTURES_REQUIRED lgp_wgsl SKIP_RETURN_CODE 77)

# Executes the generated WGSL with wgpu-native (Python package "wgpu") and
# checks pixels; skipped without the package or a Vulkan/Metal/DX12 adapter.
add_test(NAME gpu.render
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/render/render_test.py" "${LGP_WGSL_OUT}")
set_tests_properties(gpu.render PROPERTIES
    LABELS "gpu" FIXTURES_REQUIRED lgp_wgsl SKIP_RETURN_CODE 77)

# Every test shader with branches comes out as structured code
# (Options::structured, the default); lockstep compute and fs_irreducible keep
# the dispatcher, the fallback.
add_test(NAME gpu.structured_shape
    COMMAND "${Python3_EXECUTABLE}" "${_checks}" structured "${LGP_WGSL_OUT}")
set_tests_properties(gpu.structured_shape PROPERTIES LABELS "gpu" FIXTURES_REQUIRED lgp_wgsl)

# The same with the dispatcher loop { switch pc { ... } }: dynamic SSY/PBK
# stacks (LGP_WGSL_STRUCTURED=0) and static ones (Options::static_stacks, the
# pops jump straight to their targets); the images must not change.
foreach(_form dispatcher static_stacks)
    if(_form STREQUAL "dispatcher")
        set(_env "LGP_WGSL_STRUCTURED=0")
    else()
        set(_env "LGP_WGSL_STRUCTURED=0;LGP_WGSL_STATIC_STACKS=1")
    endif()
    set(_out "${CMAKE_CURRENT_BINARY_DIR}/wgsl-${_form}")
    add_test(NAME gpu.translate_${_form}
        COMMAND "${Python3_EXECUTABLE}" "${_checks}" translate $<TARGET_FILE:maxwell2wgsl> "${_out}")
    set_tests_properties(gpu.translate_${_form} PROPERTIES LABELS "gpu" FIXTURES_SETUP lgp_wgsl_${_form}
        ENVIRONMENT "${_env}")
    add_test(NAME gpu.naga_validate_${_form} COMMAND "${Python3_EXECUTABLE}" "${_checks}" naga "${_out}")
    set_tests_properties(gpu.naga_validate_${_form} PROPERTIES
        LABELS "gpu" FIXTURES_REQUIRED lgp_wgsl_${_form} SKIP_RETURN_CODE 77)
    add_test(NAME gpu.render_${_form}
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/render/render_test.py" "${_out}")
    set_tests_properties(gpu.render_${_form} PROPERTIES
        LABELS "gpu" FIXTURES_REQUIRED lgp_wgsl_${_form} SKIP_RETURN_CODE 77)
endforeach()

# Compiles the generated WGSL with Tint (Chrome) via Dawn (npm package
# "webgpu", found through LGP_WEBGPU_DIR or normal module resolution).
find_program(LGP_NODE_EXECUTABLE NAMES node nodejs)
if(LGP_NODE_EXECUTABLE)
    add_test(NAME gpu.tint_compile
        COMMAND "${LGP_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/render/tint_check.mjs" "${LGP_WGSL_OUT}")
    set_tests_properties(gpu.tint_compile PROPERTIES
        LABELS "gpu" FIXTURES_REQUIRED lgp_wgsl SKIP_RETURN_CODE 77)
    add_test(NAME gpu.tint_compile_dispatcher
        COMMAND "${LGP_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/render/tint_check.mjs"
                "${CMAKE_CURRENT_BINARY_DIR}/wgsl-dispatcher")
    set_tests_properties(gpu.tint_compile_dispatcher PROPERTIES
        LABELS "gpu" FIXTURES_REQUIRED lgp_wgsl_dispatcher SKIP_RETURN_CODE 77)
endif()
