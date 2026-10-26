# Milestone M1 Plan: Storage Engine

| Item | Value |
|---|---|
| Status | Approved (2026-10-26) |
| Design reference | `docs/DESIGN.md` 1.2 (§3–§7, §9.5, §12–§14, §17) |

M1 turns the M0 codecs into a storage engine that a C program can embed: tables are created, written, read back, and survive crashes. SQL arrives in M2, so M1 also defines the non-SQL parts of the C API that SQL will later sit on.

sxdb is a general-purpose time-series database (§0): monitoring metrics, IoT, events, and market data, from one series to hundreds of millions. M1 is validated on all of these shapes, not on one data set.

---

## 1. Exit Criteria

M1 is done when all of the following hold:

1. **Embedded read/write**: a C program opens a data directory, creates tables, writes through the appender, reads through the scan API, closes, reopens, and reads the same data.
2. **Semantics**: every type in §3.2, NULLs, partial-column writes, overwrites, and out-of-order and late writes behave as §3.3 specifies, checked against a reference model under randomized workloads with flushes and restarts at random points.
3. **Crash safety**: thousands of `kill -9` cycles and fault injection at every fsync and rename point pass the consistency check: with `fsync = always`, every acknowledged batch is present after recovery; with `1s`, recovery yields a prefix of the acknowledged batches; a batch is never partly visible.
4. **Scale**: the same code handles 1 series and at least 100 million series in one table with `memory_limit` far below the size of the series index; startup time does not depend on series count.
5. **Memory**: memory managed by sxdb never exceeds `memory_limit`; idle memory stays under 20 MB (§16).
6. **Observability**: `sxdb_status()` reports state, storage sizes, memory per pool, and average rows per block (§12.1).
7. **Formats settled**: every on-disk format M1 writes (segment, index segment, dictionaries, WAL, MANIFEST, CATALOG, FORMAT) is documented byte by byte in DESIGN or in the header of the code that writes it.
8. **CI**: unit tests, TSan, and short fuzzing runs of every new file reader pass on all CI platforms.

## 2. Scope

**In M1** (from §17): WAL, MemTable, segment files, MANIFEST, partitions, series index with active-series bitmaps, SYMBOL dictionaries, CATALOG (databases, tables, columns), buffer pool, crash recovery, `FORMAT` feature flags, storage and memory counters, `sxdb_status()`, and the embedded C API for DDL, writes, and scans.

**Not in M1**:

| Feature | Milestone | Effect on M1 |
|---|---|---|
| SQL, `sxdb_exec`, `sxdb_query_arrow` | M2 | M1 offers a typed DDL API and a scan API instead (§5) |
| Network server, line protocol, users and privileges | M3 | The CATALOG reserves record types for users and privileges but M1 writes none |
| Compaction, index segment merges, TTL, `DELETE`, bulk import, Last Cache | M4 | Segment and index segment counts grow with every flush; M1 tests accept that and measure its cost |
| Executor SIMD kernels, SMA pushdown, NUMA placement | M5 | The scan API decodes blocks; it does not aggregate |

## 3. Approach

Build a thin end-to-end path first, single-threaded and in memory where possible, then add durability, then concurrency and scale. Interface mistakes show up in the first weeks instead of at the end, and every later phase is tested against a path that already works.

Every step starts by writing its format or interface into DESIGN (or a code header for byte layouts) for review, then implements it with unit tests. A step is committed only after review.

### Phase A: single-threaded storage core

| Step | Content | Verified by |
|---|---|---|
| A1 Foundations | `os` file layer: open, pread, pwrite, O_DIRECT / F_NOCACHE, fdatasync, `flock` for `LOCK`; atomic file replacement (temp file → fsync → rename → fsync directory); a fault-injection hook at every fsync, rename, and write, compiled into test builds only. Roaring bitmap (64-bit, in-house), Bloom filter, `FORMAT` file with compat / ro_compat / incompat flags (§14.2). | Unit tests; fault-hook self-test |
| A2 Segment files | Block formation (§4.3), chunk headers with NULL and unwritten bitmaps, data pages, metadata region (column directory, page index, block index, SMA, sparse index, series bitmap), footer (§4.4). Writer and reader, with the reader reading pages through a small interface that the buffer pool implements later. | Round-trip tests per type and encoding; libFuzzer target for the reader; `sxdb-bench` segment mode reporting bytes per row **including metadata and padding** on every validation data set (§6) |
| A3 CATALOG and MANIFEST | The shared record-log format, snapshots, `CURRENT` switching, versioned table definitions; per-table `Version` objects with reference counts; removal of unreferenced segment files at open. | Unit tests; fault injection at every commit point |
| A4 Series index and dictionaries | TAG value dictionaries, SYMBOL dictionaries, fixed-length series keys, in-memory part, hot cache, index segments (`.sxi`: forward map with sparse index and Bloom filter, reverse array, inverted bitmaps), partition active-series bitmaps (§5.1). | Unit tests from 1 to 10 million series; fuzz target for the `.sxi` reader |
| A5 End-to-end path | One MemTable shard, the appender, flush (§4.9), and the scan API with merging across segments and the MemTable (§4.10): column-wise last-write-wins, unwritten values, the single-segment fast path. No WAL yet: data is durable after an explicit `sxdb_flush()`. | Reference-model tests (§7) with flushes and reopen at random points |

### Phase B: durability

| Step | Content | Verified by |
|---|---|---|
| B1 WAL | Record format, `ROW_BATCH` with series definitions and SYMBOL dictionary additions, LZ4 above 4 KB, group commit, fsync policies, 64 MB segments, deletion once every table's `FLUSHED_LSN` has passed (§4.6). | Unit tests; fuzz target for the WAL reader; LZ4 ratio measured on real batches (§7 asks for this in M1) |
| B2 Recovery | Startup sequence (LOCK, FORMAT, CATALOG, MANIFESTs, leftover cleanup, WAL replay from the smallest `FLUSHED_LSN`, truncation of a torn tail); service states `starting`, `recovering`, `running`, `read_only` (§12.1). | Unit tests; replay idempotence tests |
| B3 Crash harness | A writer process killed with `kill -9` at random times and a checker that reopens and compares with the acknowledged batches; fault injection at every fsync and rename point; disk-full and IO-error injection leading to `read_only`. | Runs in CI briefly, and for hours on home01 |

### Phase C: concurrency, memory, and scale

| Step | Content | Verified by |
|---|---|---|
| C1 Threads | WAL thread, `write_shards` shard threads, background flush threads, visible LSN and statement-level snapshots, epoch-based reclamation for MemTable buffers read during writes (§7). | TSan; concurrent reference-model tests (writers and readers at once) |
| C2 Buffer pool and memory | 64 KB frames and larger size classes, CLOCK eviction with priority for metadata pages, a ring buffer for large scans, the reclaimer registered with the memory manager, `io_mode = direct / buffered`; `memtable_limit`, freezing, and backpressure; the order of actions under memory pressure (§6.2). | Unit tests; tests that keep sxdb's accounted memory under `memory_limit` while data far exceeds it |
| C3 Scale | 100 million series with a small `memory_limit`; tables from 2 to 1,000 columns; sparse series packed into shared blocks; long out-of-order and late-data runs across many partitions; thousands of flushes without compaction. | Tests and measurements on home01 |

### Phase D: API, observability, and validation

| Step | Content | Verified by |
|---|---|---|
| D1 Status | `sxdb_status()`, file storage counters, memory per pool, average rows per block per table and partition (§12.1). | Unit tests; idle memory measurement |
| D2 C API | §9.5 rewritten as the finalized M1 API, `include/sxdb.h` documented, an example program under `tools/`. | Example builds and runs in CI |
| D3 Measurements | The M1 measurements in §8 on every validation data set, recorded in DESIGN. | Report reviewed with the user |

Parts of Phase A without dependencies on each other (the `os` layer, Roaring bitmap, Bloom filter, FORMAT file) can be built in parallel.

## 4. Designs to Settle in M1

DESIGN leaves these open or defines them only roughly. Each is written up for review before its step starts; the decisions already taken with this plan are recorded here and go into DESIGN with their step.

| Item | Where | Decision, or what remains open | Step |
|---|---|---|---|
| Index segment format | §5.1 says "finalized in M1" | Open: byte layout of `.sxi` (forward map, sparse index, Bloom filter, reverse array, inverted bitmaps, footer) | A4 |
| Dictionary files | §4.1 | Open: record format of `tag_<column>.dict` and `sym/<column>.dict`; how a torn tail is detected; ordering against the WAL so that no WAL record or segment references a value lost in a crash | A4 |
| WAL `ROW_BATCH` payload | §4.6 | Open: column layout, written-column list, per-row written masks, series definitions, SYMBOL dictionary additions | B1 |
| CATALOG records | §4.5 | Open: record types and the encoding of databases, tables, columns, and schema versions; reserved types for users and privileges (M3) | A3 |
| `FORMAT` file | §14.2 | Open: text or binary layout; how a binary reports "requires sxdb >= x.y" | A1 |
| Types | §3.2 | Decided: M1 implements every type in §3.2, including fixed-length arrays `T[N]` and variable-length arrays `T[]` | A2 |
| Variable-length arrays `T[]` | §3.2, §4.4 | Decided: the flattened values of a 1024-row block can exceed 1024 values (and the chunk's `u16 row_count`), while encoders work on chunks of at most 1024 values, so the values sub-column of a block is stored as several chunks of at most 1024 values each. Open: the exact layout | A2 |
| `WEEK` partitions | §3.5 | Decided: ISO weeks, starting Monday 00:00 UTC | A3 |
| Default database | §8.2 | Open: the name of the database created by `sxdb_open()` on an empty directory and used when a table name has no database part. Proposed: `main`, as in SQLite and DuckDB, which is not a reserved word in PostgreSQL (unlike `default`) | A3 |
| Compaction | §4.11, M4 | Decided: stays in M4. If segment counts block testing in C3, a minimal size-tiered merge moves forward into M1 | C3 |
| C API | §9.5 says "finalized in M1" | Decided: the shape in §5. Open: the details listed at the end of §5 | D2 |

## 5. C API in M1

The public API keeps the rules of §14.4: opaque handles and setter functions, no struct layouts except the Arrow C Data Interface. Without SQL, M1 adds a typed DDL API and a scan API. M2 builds `sxdb_exec` and `sxdb_query_arrow` on the same internals; the scan API stays as a low-level public API for embedded programs that do not need SQL.

```c
sxdb_options* opts = sxdb_options_new();
sxdb_options_set_memory_limit(opts, 256u << 20);
sxdb* db = NULL;
int rc = sxdb_open("/var/lib/myapp/tsdb", opts, &db);
sxdb_options_free(opts);

sxdb_table_def* def = sxdb_table_def_new("cpu");
sxdb_table_def_add_column(def, "ts", SXDB_TYPE_TIMESTAMP);
sxdb_table_def_add_column(def, "host", SXDB_TYPE_TAG);
sxdb_table_def_add_column(def, "usage", SXDB_TYPE_DOUBLE);
sxdb_table_def_set_partition(def, SXDB_PARTITION_DAY);
rc = sxdb_create_table(db, def);
sxdb_table_def_free(def);

/* The written columns are fixed when the appender opens (§3.3). */
const char* columns[] = {"ts", "host", "usage"};
sxdb_appender* ap = NULL;
rc = sxdb_appender_open(db, "cpu", columns, 3, &ap);
rc = sxdb_appender_append_arrow(ap, &array, &schema);  /* Arrow C Data Interface batch */
rc = sxdb_appender_commit(ap);                          /* one batch: all or nothing */
sxdb_appender_close(ap);

sxdb_scan_options* scan = sxdb_scan_options_new();
sxdb_scan_options_set_columns(scan, columns, 3);
sxdb_scan_options_set_time_range(scan, lo_ns, hi_ns);  /* [lo, hi) */
sxdb_scan_options_add_tag_filter(scan, "host", hosts, n_hosts);  /* = or IN */
struct ArrowArrayStream out;
rc = sxdb_scan(db, "cpu", scan, &out);
sxdb_scan_options_free(scan);

sxdb_flush(db);   /* force a flush, for tests and clean shutdown */
sxdb_close(db);
```

Still to settle in D2: how status values are returned without exposing a struct (accessor functions on an opaque status handle), error details (a per-handle last error message with SQLSTATE), and whether the appender also gets typed per-column calls for programs that do not use Arrow.

## 6. Validation Data Sets

Each data set exercises a different shape. All are generated or kept under `~/sxdb-data/` on home01, never committed.

| Data set | Shape | Source |
|---|---|---|
| Monitoring | Many tags, 10 s interval, mostly DOUBLE fields | TSBS `devops` (cpu-only and full), from `tsbs_generate_data` |
| IoT | Irregular intervals, out-of-order rows, missing values (NULL), late data | TSBS `iot` |
| High cardinality | 1 to 100 million series, few rows each | TSBS `devops` at large scale, plus a synthetic generator for sparse series |
| High-frequency events | Nanosecond timestamps, bursts, irregular gaps, rewrites of the same (series, ts) | Synthetic generator in `sxdb-bench` |
| Wide and narrow tables | 2 to 1,000 columns, partial-column writes (unwritten bitmaps) | Synthetic generator |
| Market data | 1-minute bars of 5,571 A-share symbols (28.1 million rows) | Existing data set |

TSBS (MIT) is used only as an external tool to generate files; nothing from it is vendored.

## 7. Testing

- **Unit tests** for every module, in the existing framework.
- **Reference-model tests**: a simple in-memory model of §3.3 (a map from (series, ts) to a row with per-column last-write-wins and unwritten tracking) runs the same randomized workload as sxdb, with flushes, reopenings, and crashes at random points, and every scan is compared with the model.
- **Crash injection** (Phase B3) and **fault injection** at every fsync and rename point.
- **Fuzzing**: libFuzzer targets for the segment, index segment, dictionary, WAL, MANIFEST, CATALOG, and FORMAT readers, run briefly in CI. Corrupted files must be rejected with an error, never crash or read out of bounds.
- **Sanitizers**: ASan, UBSan, and TSan in CI (TSan matters from Phase C on).

## 8. Measurements Recorded at the End of M1

Not exit gates; they are the first end-to-end numbers and set the baseline for M5.

- Write throughput through the appender in points per second, on 1 and 8 cores (§16 target: 5 million points/s on 8 cores, measured here for the first time without the network layer).
- Flush throughput, and scan throughput in values per second for cold (from disk) and warm (buffer pool) data.
- Bytes per row and per point including all metadata, on every validation data set (§16 target: at most 2 bytes per point).
- WAL LZ4 compression ratio on real batches (§7).
- Recovery speed (GB of WAL replayed per second) and startup time against series count.
- Idle memory and peak accounted memory against `memory_limit`.

With real disk usage including indexes and metadata, the storage comparison with other databases (§16 Method) becomes meaningful at the end of M1; it is proposed then, not now.

## 9. Risks

| Risk | Mitigation |
|---|---|
| Unbounded segment counts without compaction (M4) slow reads in long tests | Measure the cost in C3; if it blocks testing, pull a minimal size-tiered merge forward from M4 |
| Concurrency bugs in the lock-free MemTable path | Single-threaded first (Phase A), then TSan and concurrent reference-model tests |
| Formats change late and invalidate test data | Formats are written up and reviewed before each step; before 1.0 no compatibility is promised (§14.1), so test data can be regenerated |
| The series index at 100 million series exceeds memory or startup budgets | A4 is tested at 10 million series early; C3 at 100 million with a small `memory_limit` |
