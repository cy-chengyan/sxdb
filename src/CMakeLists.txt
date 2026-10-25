# SPDX-License-Identifier: GPL-2.0-or-later

find_package(Threads REQUIRED)

add_library(sxdb STATIC
    common/clock.c
    common/crc32c.c
    common/error.c
    common/hash.c
    common/log.c
    common/parse.c
    common/varint.c
    os/os_file.c
    os/os_mem.c
    os/os_sysinfo.c
    mem/arena.c
    mem/mem.c
    encoding/bitpack.c
    encoding/bool_codec.c
    encoding/encoding.c
    encoding/float_codec.c
    encoding/int_codec.c
    encoding/kernels.c
    encoding/kernels_scalar.c
    encoding/string_codec.c)

# SIMD codec kernels. Each instruction set lives in its own file, compiled with the flags
# for that set only; kernels.c calls them after checking the CPU (see encoding/kernels.h).
# The x86_64 sets are the integer instructions of the x86-64-v3 and x86-64-v4 levels,
# spelled out because GCC before 11 does not know the level names.
#
# The files follow the target architecture, as kernels.c does. On macOS that is
# CMAKE_OSX_ARCHITECTURES when set, because CMAKE_SYSTEM_PROCESSOR names the host there.
# A universal build lists several: every slice then compiles every kernel file (each is
# empty outside its own architecture), and each x86_64 flag is preceded by -Xarch_x86_64,
# which passes the next argument to the x86_64 slice only.
if(SXDB_SIMD)
    if(APPLE AND CMAKE_OSX_ARCHITECTURES)
        set(sxdb_target_archs ${CMAKE_OSX_ARCHITECTURES})
    else()
        set(sxdb_target_archs ${CMAKE_SYSTEM_PROCESSOR})
    endif()
    list(LENGTH sxdb_target_archs sxdb_target_arch_count)
    foreach(sxdb_arch IN LISTS sxdb_target_archs)
        if(sxdb_arch MATCHES "^(x86_64|AMD64|amd64)$")
            set(sxdb_x86_64_v3_flags -mavx2 -mbmi -mbmi2 -mlzcnt -mpopcnt)
            set(sxdb_x86_64_v4_flags ${sxdb_x86_64_v3_flags}
                -mavx512f -mavx512cd -mavx512bw -mavx512dq -mavx512vl)
            if(sxdb_target_arch_count GREATER 1)
                list(TRANSFORM sxdb_x86_64_v3_flags PREPEND "-Xarch_x86_64;")
                list(TRANSFORM sxdb_x86_64_v4_flags PREPEND "-Xarch_x86_64;")
            endif()
            target_sources(sxdb PRIVATE encoding/kernels_avx2.c encoding/kernels_avx512.c)
            set_source_files_properties(encoding/kernels_avx2.c
                PROPERTIES COMPILE_OPTIONS "${sxdb_x86_64_v3_flags}")
            set_source_files_properties(encoding/kernels_avx512.c
                PROPERTIES COMPILE_OPTIONS "${sxdb_x86_64_v4_flags}")
            target_compile_definitions(sxdb PRIVATE SX_SIMD_KERNELS)
        elseif(sxdb_arch MATCHES "^(aarch64|arm64|ARM64)$")
            target_sources(sxdb PRIVATE encoding/kernels_neon.c)
            target_compile_definitions(sxdb PRIVATE SX_SIMD_KERNELS)
        endif()
    endforeach()
endif()

# Internal headers are reachable as "common/...", "mem/...", and so on. Only include/
# will be installed; src/ is exposed to the in-tree tests and tools.
target_include_directories(sxdb PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(sxdb
    PUBLIC Threads::Threads
    PRIVATE sxdb_build_flags)

if(NOT APPLE)
    target_link_libraries(sxdb PUBLIC m)
endif()
