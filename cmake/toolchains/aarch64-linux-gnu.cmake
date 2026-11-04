# AArch64 Linux cross build, run under qemu-user (docs/knowledge/originalcode-nativ-ausfuehren.md):
# tests of native guest-code execution on an x86 machine.
#   apt install g++-aarch64-linux-gnu qemu-user-static
#   cmake -S . -B build/a64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux-gnu.cmake ...
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
find_program(LGP_QEMU_AARCH64 NAMES qemu-aarch64-static qemu-aarch64)
if(LGP_QEMU_AARCH64)
    set(CMAKE_CROSSCOMPILING_EMULATOR "${LGP_QEMU_AARCH64};-L;/usr/aarch64-linux-gnu")
endif()
