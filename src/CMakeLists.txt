# SPDX-License-Identifier: GPL-2.0-or-later

find_package(Threads REQUIRED)

add_library(sxdb STATIC
    common/allocator.c
    common/bloom.c
    common/clock.c
    common/crc32c.c
    common/error.c
    common/hash.c
    common/log.c
    common/parse.c
    common/roaring.c
    common/roaring_container.c
    common/varint.c
    common/xxhash.c
    os/os_fault.c
    os/os_file.c
    os/os_mem.c
    os/os_random.c
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
    encoding/string_codec.c
    storage/data_dir.c
    storage/format.c)

# The target architectures. On macOS that is CMAKE_OSX_ARCHITECTURES when set, because
# CMAKE_SYSTEM_PROCESSOR names the host there. A universal build lists several, and each
# x86_64-only flag is then preceded by -Xarch_x86_64, which passes the next argument to the
# x86_64 slice only.
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    set(sxdb_target_archs ${CMAKE_OSX_ARCHITECTURES})
else()
    set(sxdb_target_archs ${CMAKE_SYSTEM_PROCESSOR})
endif()
list(LENGTH sxdb_target_archs sxdb_target_arch_count)

# Intel cores from Skylake to Cascade Lake with the microcode update for their jump
# conditional code (JCC) erratum do not keep a jump that crosses or ends on a 32-byte
# boundary in the decoded-instruction cache. A hot loop can then run up to 25% slower only
# because of where the linker puts it, as the codec kernels did. The assembler pads such
# jumps away for about 2.5% more code; other CPUs are unaffected. Clang takes the option
# directly, GCC passes it to the GNU assembler (binutils 2.34 and later).
#
# The same cache delivers the instructions of one 32-byte window per cycle, so a small loop
# also runs at a speed that depends on how it falls across those windows. Aligning every
# function to 64 bytes fixes the place of each loop at compile time: link order and
# changes elsewhere in the library no longer move codec loops across windows, which made
# equal code 7 to 24% faster or slower between builds on a Xeon Platinum 8260.
if(sxdb_target_archs MATCHES "(^|;)(x86_64|AMD64|amd64)(;|$)")
    include(CheckCCompilerFlag)
    if(sxdb_target_arch_count GREATER 1)
        # Universal builds need Apple clang, which knows the options.
        target_compile_options(sxdb PRIVATE "SHELL:-Xarch_x86_64 -mbranches-within-32B-boundaries"
                                            "SHELL:-Xarch_x86_64 -falign-functions=64")
    else()
        check_c_compiler_flag(-falign-functions=64 SXDB_HAVE_FUNCTION_ALIGNMENT)
        if(SXDB_HAVE_FUNCTION_ALIGNMENT)
            target_compile_options(sxdb PRIVATE -falign-functions=64)
        endif()
        check_c_compiler_flag(-mbranches-within-32B-boundaries SXDB_HAVE_BRANCH_ALIGNMENT)
        if(SXDB_HAVE_BRANCH_ALIGNMENT)
            target_compile_options(sxdb PRIVATE -mbranches-within-32B-boundaries)
        else()
            check_c_compiler_flag(-Wa,-mbranches-within-32B-boundaries
                SXDB_HAVE_ASSEMBLER_BRANCH_ALIGNMENT)
            if(SXDB_HAVE_ASSEMBLER_BRANCH_ALIGNMENT)
                target_compile_options(sxdb PRIVATE -Wa,-mbranches-within-32B-boundaries)
            endif()
        endif()
    endif()
endif()

# SIMD codec kernels. Each instruction set lives in its own file, compiled with the flags
# for that set only; kernels.c calls them after checking the CPU (see encoding/kernels.h).
# The x86_64 sets are the integer instructions of the x86-64-v3 and x86-64-v4 levels,
# spelled out because GCC before 11 does not know the level names.
#
# The files follow the target architecture, as kernels.c does. A universal build compiles
# every kernel file in every slice (each is empty outside its own architecture).
if(SXDB_SIMD)
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
