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
if(SXDB_SIMD)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        target_sources(sxdb PRIVATE encoding/kernels_avx2.c encoding/kernels_avx512.c)
        set_source_files_properties(encoding/kernels_avx2.c
            PROPERTIES COMPILE_OPTIONS "-mavx2")
        set_source_files_properties(encoding/kernels_avx512.c
            PROPERTIES COMPILE_OPTIONS "-mavx512f;-mavx512dq")
        target_compile_definitions(sxdb PRIVATE SX_SIMD_KERNELS)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
        target_sources(sxdb PRIVATE encoding/kernels_neon.c)
        target_compile_definitions(sxdb PRIVATE SX_SIMD_KERNELS)
    endif()
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
