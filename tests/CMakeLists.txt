# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(sxdb-unit-tests
    unit/test_main.c
    unit/test_common.c
    unit/test_log.c
    unit/test_mem.c
    unit/test_bitpack.c
    unit/test_int_codec.c
    unit/test_float_codec.c
    unit/test_bool_string_codec.c
    unit/test_codec_kernels.c)

target_link_libraries(sxdb-unit-tests PRIVATE sxdb sxdb_build_flags)

set(sxdb_unit_suites
    common
    log
    mem
    bitpack
    int_codec
    float_codec
    bool_string_codec
    codec_kernels)

foreach(suite IN LISTS sxdb_unit_suites)
    add_test(NAME unit.${suite} COMMAND sxdb-unit-tests ${suite})
endforeach()

# The codec suites run once more with the portable kernels; by default they use the best
# kernel set of the machine. codec_kernels compares every supported set with the portable one.
foreach(suite IN ITEMS bitpack int_codec float_codec bool_string_codec)
    add_test(NAME unit.${suite}.scalar COMMAND sxdb-unit-tests --simd scalar ${suite})
endforeach()
