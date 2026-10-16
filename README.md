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

`scripts/check-source.sh` checks the source rules (ASCII only, banned functions, license headers, layering). Formatting follows `.clang-format`.

## Encoding Benchmark

`sxdb-bench` measures the compression ratio and the encode and decode speed of the column encodings:

```sh
build/tools/sxdb-bench/sxdb-bench                 # all built-in synthetic datasets
build/tools/sxdb-bench/sxdb-bench --list          # list the datasets
build/tools/sxdb-bench/sxdb-bench --csv data.csv --column 2 --type double --skip-header
```

The `--csv` form runs the same measurement on one column of your own data.

## Documentation

- [Design document](docs/DESIGN.md)

## License

sxdb is licensed under the GNU General Public License, version 2 or (at your option) any later version (GPL-2.0-or-later). The Python client will be licensed under the MIT License.
