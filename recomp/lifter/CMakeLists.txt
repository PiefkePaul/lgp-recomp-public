# A64 decoder and A64 -> C++ lifter (roadmap P2.3, SIMD&FP: P2.4). Plain C++20
# without third-party dependencies, so the WebAssembly preset builds it too.
add_library(lgp_recomp_lifter STATIC
    src/a64_decode.cpp
    src/lifter.cpp
    # SIMD&FP decoder/lifter (P2.4)
    src/simd/simd_lift.cpp
    src/simd/crypto.cpp
    src/simd/fp_scalar.cpp
    src/simd/vec_int.cpp
    src/simd/vec_fp.cpp
    src/simd/vec_perm.cpp
    src/simd/ldst_struct.cpp)
target_include_directories(lgp_recomp_lifter PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(lgp_recomp_lifter PUBLIC cxx_std_20)

# Header-only support code that generated (lifted) C++ includes.
add_library(lgp_lifted_headers INTERFACE)
target_include_directories(lgp_lifted_headers INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(lgp_lifted_headers INTERFACE cxx_std_20)
# P6 measurement: count FP operations on the software path (fp.hpp, printed by lgp_game).
option(LGP_FP_STATS "Count FP operations that take the software path" OFF)
if(LGP_FP_STATS)
    target_compile_definitions(lgp_lifted_headers INTERFACE LGP_FP_STATS=1)
endif()
# LGP_FP_HOST_NAN (P6, used by lgp_game in runtime/boot) is defined in the
# top-level CMakeLists.txt.
# Guest FP semantics need IEEE-exact host arithmetic: no FMA contraction of
# a*b+c (GCC contracts by default in gnu++ mode, Clang within expressions on
# targets with FMA such as arm64 macOS) and no fast-math.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(lgp_lifted_headers INTERFACE -ffp-contract=off -fno-fast-math)
elseif(MSVC)
    target_compile_options(lgp_lifted_headers INTERFACE /fp:precise)
endif()
