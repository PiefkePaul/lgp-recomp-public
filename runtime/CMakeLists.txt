add_subdirectory(core)
# Native execution of the rewritten original code (AArch64 hosts, docs/analysis/56).
add_subdirectory(native)
add_subdirectory(hle)
# Game mods on hook doors of the lifted code (P8).
add_subdirectory(mods)
# Boot program; on Emscripten it reads the user's data from OPFS (P6.1).
add_subdirectory(boot)
