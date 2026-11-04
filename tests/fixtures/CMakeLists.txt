# Homebrew test fixtures, cross-compiled with devkitA64 + libnx.
#
# This is a separate CMake project: it needs the devkitPro Switch toolchain
# ($DEVKITPRO/cmake/Switch.cmake). The main project builds it as an external
# project when LGP_BUILD_FIXTURES is ON (see tests/CMakeLists.txt); CI does
# that inside the pinned devkitpro/devkita64 container.
#
# Outputs (in this project's binary dir, never committed):
#   basic.elf   ELF with full symbols (reference for function boundaries)
#   basic.nro   homebrew NRO (elf2nro, own NACP, no icon)
#   basic.nso   same ELF as NSO (elf2nso, LZ4-compressed segments)
#   threads.{elf,nro,nso}  the same for the multi-threading fixture
#   services.{elf,nso}     libnx service clients against the HLE services (P3.4)
cmake_minimum_required(VERSION 3.25)

project(lgp_fixtures VERSION 0.0.1 LANGUAGES C ASM)

if(NOT NINTENDO_SWITCH)
    message(FATAL_ERROR "tests/fixtures needs the devkitPro toolchain: "
                        "-DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake")
endif()

set(CMAKE_C_STANDARD 11)

add_executable(basic basic/main.c basic/asm_ops.s)
target_compile_options(basic PRIVATE -O2 -Wall -Wextra)
# Keep the ELF reproducible: no build paths or timestamps leak into it.
target_compile_options(basic PRIVATE "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.")

nx_generate_nacp(OUTPUT basic.nacp NAME "lgp fixture basic" AUTHOR "lgp-recomp" VERSION 0.0.1)
nx_create_nro(basic NOICON NACP basic.nacp OUTPUT basic.nro)

add_custom_command(
    OUTPUT basic.nso
    COMMAND "${NX_ELF2NSO_EXE}" "$<TARGET_FILE:basic>" basic.nso
    DEPENDS basic
    COMMENT "Converting basic to NSO"
    VERBATIM)
add_custom_target(basic_nso ALL DEPENDS basic.nso)

add_executable(threads threads/main.c threads/page16k.c)
target_compile_options(threads PRIVATE -O2 -Wall -Wextra "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.")
# Like Nintendo SDK modules (and unlike devkitA64's default), with
# .eh_frame_hdr, so function discovery gets its FDE seeds. basic stays without
# and covers the heuristic seeds.
target_link_options(threads PRIVATE -Wl,--eh-frame-hdr)
# Room between .text and .data for 16 KiB host pages (threads/page16k.c).
target_link_options(threads PRIVATE -Wl,-u,lgp_fixture_page16k)
nx_generate_nacp(OUTPUT threads.nacp NAME "lgp fixture threads" AUTHOR "lgp-recomp" VERSION 0.0.1)
nx_create_nro(threads NOICON NACP threads.nacp OUTPUT threads.nro)
add_custom_command(
    OUTPUT threads.nso
    COMMAND "${NX_ELF2NSO_EXE}" "$<TARGET_FILE:threads>" threads.nso
    DEPENDS threads
    COMMENT "Converting threads to NSO"
    VERBATIM)
add_custom_target(threads_nso ALL DEPENDS threads.nso)

# libnx's own service clients (tests/boot runs it through the boot program
# against the HLE services). NSO only: an NRO reads its RomFS through sdmc.
add_executable(services services/main.c)
target_compile_options(services PRIVATE -O2 -Wall -Wextra "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.")
target_link_options(services PRIVATE -Wl,--eh-frame-hdr)
add_custom_command(
    OUTPUT services.nso
    COMMAND "${NX_ELF2NSO_EXE}" "$<TARGET_FILE:services>" services.nso
    DEPENDS services
    COMMENT "Converting services to NSO"
    VERBATIM)
add_custom_target(services_nso ALL DEPENDS services.nso)

# Reference data from binutils for the inventory test: relocations and build ID
# as the toolchain itself sees them in the ELF.
if(NOT CMAKE_READELF)
    find_program(CMAKE_READELF NAMES aarch64-none-elf-readelf HINTS "${DEVKITPRO}/devkitA64/bin" REQUIRED)
endif()
add_custom_command(
    OUTPUT basic.readelf.txt
    COMMAND "${CMAKE_READELF}" --wide --relocs --notes "$<TARGET_FILE:basic>" > basic.readelf.txt
    DEPENDS basic
    COMMENT "Dumping relocations and notes of basic.elf"
    VERBATIM)
add_custom_target(basic_readelf ALL DEPENDS basic.readelf.txt)
