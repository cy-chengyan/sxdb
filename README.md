# sxdb

sxdb is a single-node time-series database written in pure C. It aims for high write and query performance while keeping memory usage strictly bounded.

> **Status:** early development (milestone M0). The base libraries, logging, memory manager, column encodings, and an encoding benchmark exist. There is no storage engine, SQL, or server yet, so sxdb cannot store data. The design is described in [docs/DESIGN.md](docs/DESIGN.md).

## Planned Features

- **Two forms from one code base**: a standalone server (`sxdb-server`) and an embeddable library (`libsxdb`).
- **SQL** in the PostgreSQL dialect, with time-series extensions such as `time_bucket`, gap filling, `ASOF JOIN`, window functions, and continuous aggregates.
- **Columnar storage** with lightweight, SIMD-friendly encodings (delta, frame-of-reference bit-packing, ALP for floating point) and block-level pre-aggregates.
- **Bounded memory**: one fixed `memory_limit`, fully used as cache but never exceeded.
- **Protocols**: PostgreSQL wire protocol (psql, Grafana, PG drivers), InfluxDB line protocol (Telegraf), HTTP with JSON/CSV, and Arrow IPC for fast pandas and polars access.
- **Data types for many workloads**, from monitoring and IoT to financial market data: `DECIMAL`, `SYMBOL`, and fixed or variable-length arrays.
- **Backups**: online full and incremental backups, WAL archiving, and point-in-time recovery.
- **Secure defaults**: refuses to run as root, listens on localhost only, and requires authentication with no default password.
- **Platforms**: Linux and macOS on x86_64 and ARM64.

## Building

Requirements: a C17 compiler (GCC 8+, Clang 10+, or Apple Clang 12+) and CMake 3.20+.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Useful options:

| Option | Effect |
|---|---|
| `-DSXDB_SANITIZE=address,undefined` | Build with AddressSanitizer and UndefinedBehaviorSanitizer (use with `-DCMAKE_BUILD_TYPE=Debug`) |
| `-DSXDB_SANITIZE=thread` | Build with ThreadSanitizer |
| `-DSXDB_WERROR=ON` | Treat compiler warnings as errors (always on in CI) |
| `-DSXDB_SIMD=OFF` | Build only the portable codec kernels, without the AVX2 (x86-64-v3), AVX-512 (x86-64-v4), and NEON ones (by default the best set the CPU supports is chosen at run time) |
| `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"` | On macOS, build a universal binary, or name one architecture to build for it alone; each architecture gets its own SIMD kernels |

`scripts/check-source.sh` checks the source rules (ASCII only, banned functions, license headers, layering). Formatting follows `.clang-format` and is checked in CI with clang-format 22.1.8; other releases format some constructs differently, so use the same version (`pipx install clang-format==22.1.8`).

## Encoding Benchmark

`sxdb-bench` measures the compression ratio and the encode and decode speed of the column encodings:

```sh
build/tools/sxdb-bench/sxdb-bench                 # all built-in synthetic datasets
build/tools/sxdb-bench/sxdb-bench --list          # list the datasets
build/tools/sxdb-bench/sxdb-bench --csv bars/ --columns 2:timestamp,3:decimal2,7:decimal0 --split-on 9
```

The `--csv` form measures columns of your own data. Each path is a CSV file or a directory of them, and every file is treated as one series. Column types are `int64`, `double`, `bool`, `string`, `timestamp`, and `decimalN` (N fraction digits). `--split-on FIELD` starts a new chunk whenever that field changes, which mimics a partition such as one trading day. `--threads N` measures with N threads. `--simd LEVEL` measures one codec kernel set (`scalar`, `avx2`, `avx512`, or `neon`) instead of the best one the CPU supports. Run `sxdb-bench --help` for all options.

## Documentation

- [Design document](docs/DESIGN.md)

## License

sxdb is licensed under the GNU General Public License, version 2 or (at your option) any later version (GPL-2.0-or-later). The Python client will be licensed under the MIT License.
