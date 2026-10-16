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
    encoding/string_codec.c)

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
