# SPDX-License-Identifier: GPL-2.0-or-later
#
# Compiler and linker flags shared by every sxdb target. Targets opt in by linking
# the sxdb_build_flags interface library.

add_library(sxdb_build_flags INTERFACE)

target_compile_options(sxdb_build_flags INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wstrict-prototypes
    -Wmissing-prototypes
    -Wformat=2
    -Wvla
    # ALP verifies that every encoded double decodes bit-exactly. Fused multiply-add
    # contraction would make encoder and decoder arithmetic differ, so it is disabled.
    -ffp-contract=off
    -fstack-protector-strong)

if(SXDB_WERROR)
    target_compile_options(sxdb_build_flags INTERFACE -Werror)
endif()

# POSIX and platform feature macros. The code base builds with -std=c17 (no GNU
# extensions), so system interfaces beyond ISO C must be requested explicitly.
if(APPLE)
    target_compile_definitions(sxdb_build_flags INTERFACE _DARWIN_C_SOURCE)
else()
    target_compile_definitions(sxdb_build_flags INTERFACE _GNU_SOURCE)
endif()

# _FORTIFY_SOURCE needs optimization, so it is only added to optimized builds.
# Level 3 needs GCC 12 or Clang; older compilers get level 2.
set(sxdb_fortify_level 3)
if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_C_COMPILER_VERSION VERSION_LESS 12)
    set(sxdb_fortify_level 2)
endif()
if(NOT SXDB_SANITIZE)
    target_compile_options(sxdb_build_flags INTERFACE
        $<$<NOT:$<CONFIG:Debug>>:-U_FORTIFY_SOURCE>
        $<$<NOT:$<CONFIG:Debug>>:-D_FORTIFY_SOURCE=${sxdb_fortify_level}>)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(sxdb_build_flags INTERFACE -Wl,-z,relro -Wl,-z,now)
endif()

if(SXDB_SANITIZE)
    target_compile_options(sxdb_build_flags INTERFACE
        -fsanitize=${SXDB_SANITIZE}
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all)
    target_link_options(sxdb_build_flags INTERFACE -fsanitize=${SXDB_SANITIZE})
endif()
