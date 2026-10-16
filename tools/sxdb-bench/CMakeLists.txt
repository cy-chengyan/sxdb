# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(sxdb-bench
    main.c
    column.c
    csv.c
    datasets.c)

target_link_libraries(sxdb-bench PRIVATE sxdb sxdb_build_flags)
