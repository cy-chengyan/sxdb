# sxdb Design Document

| Item | Value |
|---|---|
| Document version | 1.2 |
| Status | Milestone M0 (foundation and encodings) in progress |
| Date | 2026-10-17 |
| License | GPL-2.0-or-later (the Python client is MIT) |

This document is the design of sxdb; version 1.0 was the baseline, and later changes are tracked in version control. Open items are listed in §18.

---

## 0. Goals and Non-Goals

### Goals
- A single-node time-series database written in pure C, shipped both as a standalone server (`sxdb-server`) and as an embeddable library (`libsxdb`).
- High write and query performance. Memory usage is strictly bounded by `memory_limit` and fully used within that bound.
- SQL: the PostgreSQL dialect plus time-series extensions.
- Online full backups, incremental backups, and point-in-time recovery.
- Broad applicability: monitoring metrics, IoT, financial market data (ticks, OHLC bars, order book snapshots), and more. No assumptions about series count, write rate, or table width; everything from 1 series to hundreds of millions of series must work.
- Security is a first-class requirement.

### Non-Goals
- Distribution, replication, clustering.
- Full ANSI SQL, multi-statement transactions, `UPDATE`.
- Windows and big-endian platforms.
- Backups to object storage (S3 and similar) and backup encryption.
- Dedicated tools for migrating data from other databases (not in the first release; to migrate, export to CSV or Arrow and import).

### Trade-off Principles
1. Performance takes priority over storage space.
2. Memory is strictly bounded by `memory_limit`. Within that bound, memory is used as fully as possible, mainly as cache.
3. Defaults must be secure: never run as root, listen on localhost only, require authentication.

---

## 1. Ground Rules

| Item | Rule |
|---|---|
| Language | C17 (`-std=c17`). Minimum GCC 8, Clang 10, Apple Clang 12. CI also builds with `-std=c23` to stay forward compatible |
| Platforms | Linux and macOS; x86_64 and ARM64; little-endian only, checked at compile time with `static_assert` |
| License | Server and libsxdb: GPL-2.0-or-later; Python client: MIT. Every source file carries an `SPDX-License-Identifier` |
| Contributions | DCO (commits carry `Signed-off-by`) |
| Third-party code | Only GPLv2-compatible licenses (BSD, MIT, zlib, public domain, LGPL). **No Apache-2.0 code** |
| Vendored dependencies | LZ4 (only `lib/`, BSD-2-Clause), ZSTD (BSD or GPLv2 dual license), linenoise (BSD-2-Clause, sxql only) |
| Optional dependency | Mbed TLS (Apache-2.0 or GPL-2.0-or-later dual license), for TLS |
| Written in-house | Roaring Bitmap, t-digest, HyperLogLog, ALP, Arrow IPC encoding (including a minimal FlatBuffers encoder), TZif time zone parsing, SHA-256, HMAC, PBKDF2 (for SCRAM), shortest round-trip floating-point formatting (Ryu-style) and fast exact number parsing (Eisel–Lemire-style) |
| Compiler extensions | `__builtin_*`, `__attribute__`, etc. are wrapped in `sx_compiler.h`. C23 features (`[[nodiscard]]`, `<stdbit.h>`, etc.) are used through compatibility macros |
| Human language | Error messages, logs, and all other system text are English only. Code comments and repository documentation are English only (see §15.2) |
| Hosting | GitHub (see §15.3) |

---

## 2. Architecture

```
 sxql / psql / Grafana   Telegraf / collectors   pandas / polars        C programs
        │ PG wire v3             │ line protocol         │ Arrow IPC (HTTP)     │ libsxdb API
        ▼                        ▼                       ▼                      ▼
┌────────────────────────────────────────────────────────────────────────────────────────────┐
│ Network     IO threads (epoll / kqueue), authentication, protocol parsing                  │
├────────────────────────────────────────────────────────────────────────────────────────────┤
│ SQL         parse → bind → logical plan → rule-based optimizer → physical plan             │
│ Execution   vectorized operators (Arrow layout, 1024 rows/vector), morsel parallelism      │
├────────────────────────────────────────────────────────────────────────────────────────────┤
│ Storage engine                                                                             │
│   write       WAL (group commit) → MemTable (sharded) → flush → immutable segments         │
│   read        series index → partition pruning → block index / SMA → buffer pool → decode  │
│   background  compaction / TTL / WAL archiving / view refresh                              │
├────────────────────────────────────────────────────────────────────────────────────────────┤
│ Memory manager (memory_limit)       Backup & restore (full / incremental / PITR)           │
└────────────────────────────────────────────────────────────────────────────────────────────┘
```

`libsxdb` is the core: SQL, execution, storage, memory management, and backup. `sxdb-server` adds the network layer on top. Both forms share one code base.

---

## 3. Data Model

### 3.1 Tables and Series

```sql
CREATE TABLE cpu (
    ts      TIMESTAMP,      -- time column
    host    TAG,            -- tag: one combination of tag values = one series
    region  TAG,
    usage   DOUBLE,         -- value columns
    temp    FLOAT,
    status  INT
) PARTITION BY DAY TTL 90d;
```

- The first `TIMESTAMP` column of a table is its time column. Every table must have one.
- One combination of TAG values forms a **series**. Each series gets a 64-bit `series_id` within its table, assigned in creation order and never reused. Tag values are stored once in the series index, not repeated on every row (see §5.1).
- A table without TAG columns is a single series.

### 3.2 Types

| Type | Internal representation | Notes |
|---|---|---|
| `BOOL` | bit | |
| `INT` / `BIGINT` | int32 / int64 | |
| `FLOAT` / `DOUBLE` | IEEE 754 | |
| `DECIMAL(p,s)`, p ≤ 18 | scaled int64 | Exact arithmetic for prices and amounts |
| `TIMESTAMP` | UTC int64 nanoseconds | Range 1677-09-21 to 2262-04-11. One precision for all tables, so joins never convert |
| `VARCHAR` | UTF-8 | |
| `SYMBOL` | table-level dictionary | Low-cardinality string values (for example trade side). **Not** part of series identity |
| `TAG` | dictionary in the series index | Defines series identity |
| `T[N]` | N sub-columns | Fixed-length array, for example 10 order book levels |
| `T[]` | offsets column + flattened values column | Variable-length array, same layout as Arrow List |

Array elements can be numeric, `DECIMAL`, `BOOL`, or `TIMESTAMP`. Subscripts start at 1 and `[1:5]` slicing is supported, as in PostgreSQL.

**Prices and amounts should use `DECIMAL`, not `DOUBLE`.** Arithmetic is exact, sums do not accumulate rounding error, and the data is smaller: on A-share 1-minute bars, a table with `DECIMAL` prices took 9.63 bytes per row against 10.71 with `DOUBLE` (MONTH partitions), about 10% less. Documentation examples and client type mappings follow this recommendation.

```sql
CREATE TABLE l2_snapshot (
    ts       TIMESTAMP,
    symbol   TAG,
    bid_px   DECIMAL(18,4)[10],
    bid_vol  BIGINT[10],
    ask_px   DECIMAL(18,4)[10],
    ask_vol  BIGINT[10]
) PARTITION BY DAY;
```

A fixed-length array is stored as N sub-columns. Each level compresses on its own (prices at the same level change slowly, so they compress well), and a query on `bid_px[1]` reads only that sub-column.

### 3.3 Write Semantics
- Out-of-order writes are allowed.
- **For the same (series, ts), a later write overwrites an earlier one.** All tables use this rule; there is no per-table switch.
- **Overwrite granularity: the columns that are written are the columns that are overwritten.** Columns not written keep their old values. Writing an explicit NULL counts as writing, so it overwrites the old value with NULL. The semantics match PostgreSQL's `INSERT ... ON CONFLICT DO UPDATE SET <written columns> = EXCLUDED.<column>`.
- An array column is overwritten as a whole, never merged element by element.
- A column that has never been written reads as NULL.
- **Note for writers**: a series keeps only the last value written at a given timestamp. If several events can share a timestamp (for example several trades in the same millisecond), the writer must make timestamps unique, for example by encoding a sequence number into the low nanosecond digits.
- `UPDATE` is not supported. To change some columns, write the same (series, ts) with only those columns.

Which columns count as written, per write path:

| Write path | Columns that count as written |
|---|---|
| `INSERT INTO t (ts, symbol, close) VALUES ...` | The columns in the column list |
| `INSERT INTO t VALUES ...` (no column list) | All columns, which amounts to replacing the whole row |
| Line protocol | The fields present on that line |
| Arrow, `COPY` | The columns present in the batch |
| C API appender | The columns given when the appender is opened |

```
first write:  ts=09:30:00, symbol=A, open=10.0, close=10.5, volume=1000
second write: ts=09:30:00, symbol=A, close=10.6, volume=NULL     <- writes close and volume
result:       open=10.0, close=10.6, volume=NULL
```

- **Batch atomicity**: one `INSERT`, one HTTP write request, or one `COPY` batch becomes visible entirely or not at all.

### 3.4 Deletion and Expiry
- `DELETE FROM t WHERE <time range> [AND <TAG conditions>]`: the first release supports only these two kinds of conditions. A delete is recorded as a tombstone (series set × time range), filtered out at query time, and physically removed during compaction.
- TTL: once a whole partition has expired, its directory is deleted.
- `ALTER TABLE t DROP PARTITION WHERE ts < ...`: drop partitions manually.

### 3.5 Partitioning
Tables are partitioned by time, with a granularity of `HOUR`, `DAY`, `WEEK`, `MONTH`, or `YEAR`. Boundaries are in UTC.

**Choosing the granularity**: a typical series should have at least 1024 rows per partition. Blocks are then full single-series blocks (see §4.3), which compress best and which queries read and skip with the least per-block overhead.

| Data | Rows per series | Suggested granularity |
|---|---|---|
| Ticks, high-frequency metrics (1 s or faster) | Thousands or more per day | `DAY` (or `HOUR`) |
| Monitoring metrics every 10–60 s | 1,440–8,640 per day | `DAY` |
| 1-minute bars | About 240 per trading day | `MONTH` |
| Daily bars | About 250 per year | `YEAR` |

Measured on A-share 1-minute bars (5,571 symbols, 241 rows per symbol per day), `MONTH` partitions took 10.71 bytes per row against 12.30 with `DAY` partitions, 13% less. The average number of rows per block is reported per table (see §12.1), so a granularity that is too fine shows up directly.

---

## 4. Storage Engine

### 4.1 On-Disk Layout

```
<data_dir>/                          directories 0700, files 0600
├── LOCK                             exclusive process lock (flock); one process per data directory
├── FORMAT                           data directory format version and enabled feature flags (see §14)
├── CATALOG/
│   ├── CURRENT                      points to the current CATALOG file
│   └── CATALOG-000003               databases, tables, columns, users, privileges (version log)
├── wal/
│   └── 000000000001.wal ...         write-ahead log, 64 MB per segment, one WAL per instance
└── <db>/<table>/
    ├── CURRENT
    ├── MANIFEST-000005              per-table version log
    ├── series/                      series index (see §5.1)
    │   ├── tag_<column>.dict        TAG value dictionary (append-only)
    │   └── 000004.sxi               index segment (immutable, covers a contiguous series_id range)
    ├── sym/<column>.dict            SYMBOL dictionary (append-only)
    └── p20261016/                   time partition
        ├── 000017.sxs               segment file (immutable)
        └── 000023.sxs
```

### 4.2 Encodings

All encodings are lightweight and SIMD-decodable. Following the performance-first principle, bit-serial encodings such as Gorilla and Chimp are not used.

| ID | Encoding | Used for |
|---|---|---|
| 0 | RAW | Fallback when nothing else is smaller |
| 1 | CONSTANT | A whole block with one value, including the series_id column of single-series blocks |
| 2 | RLE | series_id, BOOL, slowly changing values |
| 3 | FOR_BITPACK | Integers |
| 4 | DELTA_FOR | Timestamps, integers, DECIMAL. A common divisor is factored out per block, so millisecond data stored as nanoseconds costs no extra space |
| 5 | DELTA_CONST | Regular timestamps: only start and step are stored, and row positions can be computed directly |
| 6 | ALP | DOUBLE, FLOAT (ALP, SIGMOD 2024) |
| 7 | ALP_RD | High-precision floating point (when ALP does not apply) |
| 8 | DICT | Table-level dictionary ids for SYMBOL; per-block dictionary for VARCHAR |
| 9 | STRING | Offsets + bytes, optionally LZ4 |
| 10 | BITMAP | BOOL |
| 11 | DELTA_PATCHED | Timestamps with a few large gaps (lunch breaks, nights, weekends) and other mostly regular sequences: regular steps are packed narrowly and the few large ones are stored apart as exceptions |

- When writing a chunk, the applicable encodings are compared and the smallest result wins. Integer encodings are sized exactly from one pass of statistics; floating-point encodings are estimated from a sample.
- ALP parameter choice is carried across the chunks of a column: the encoder remembers the best exponent/factor pairs, repeats the full search every 64 chunks or when the remembered pairs start producing exceptions, and always also tries fewer decimal places. This keeps encoding fast on small chunks without losing compression.
- Bit-packing uses a FastLanes-style interleaved layout of 1024 values, so even scalar code auto-vectorizes.
- Cold partitions can add a ZSTD layer, enabled with the table option `cold_compression = 'zstd'` (off by default).
- The byte layout of each encoding's payload is documented in its codec header (`src/encoding/*_codec.h`).

**Codec speed** (targets in §16)
- **Fused decoding**: unpacking, adding the FOR base and multiplier, and the ALP multiplication run in one pass over each 1024-value vector, so values stay in registers instead of making several passes through memory.
- **Runtime SIMD dispatch**: the hot loops of the codecs (bit-packing, FOR and ALP decoding, rebuilding values from deltas, ALP encoding, and the statistics that choose an integer encoding: value and delta ranges, common-divisor tests with exact division, bit-width histograms, finding DELTA_PATCHED exceptions, reordering deltas into lanes) are kernels with a portable C version plus versions for the x86-64-v3 (AVX2) and x86-64-v4 (AVX-512 F, CD, BW, DQ, VL) levels on x86_64 and a NEON version on ARM64 (`src/encoding/kernels.h`). Each instruction set lives in its own file compiled with only its flags, and the set is chosen once, on first use, from the CPU features; the executor uses the same mechanism (see §8.1). The kernels are written with intrinsics because compilers do not vectorize these loops reliably: GCC 11 left the bit-unpacking loop scalar even with `-march=native`. The portable kernels are the reference: tests require every SIMD kernel to produce exactly the same output, including on random and corrupted payloads. `-DSXDB_SIMD=OFF` builds only the portable kernels.
- **Branch-free encoding**: ALP encodes a whole vector, decodes it back, and compares the two to find exceptions, instead of testing each value as it goes. Integers and doubles are converted with the 2^52 + 2^51 identity instead of conversion instructions, which SSE2, AVX2, and NEON lack for 64-bit integers, so every kernel performs the same floating-point operations; the payload format is unchanged.
- **Parallel delta decoding** (adopted in M0): DELTA_FOR and DELTA_PATCHED pack their deltas in a lane-contiguous order (delta i in lane i / rows, see `src/encoding/bitpack.h`), a variant of the FastLanes transposed layout. The 16 lanes compute independent prefix sums, each starting after the sum of the lanes before it, and a transposition in registers restores the original order. Decoding takes two passes over a scratch array: the first decodes the deltas like FOR and adds up each lane, DELTA_PATCHED then replaces the deltas of its exceptions and corrects the lane sums, and the second runs the prefix sums from the lane starts and transposes. A zero row in front of the deltas turns the first value and the lane boundaries into ordinary positions, and the decoder may write the padding of the last row, as FOR decoding does, so the second pass needs no per-lane bookkeeping; when the packed width is 0 (as for the timestamps of the 1-minute bars) the multiplication is skipped. Moving the prefix sums into the first pass and leaving only the transposition for the second was faster for DELTA_FOR with AVX-512, but it needs the exceptions inside the first pass and was slower for DELTA_PATCHED, which covers nearly all timestamp blocks and half the price blocks of the 1-minute bars. Unlike FastLanes, no per-lane base values are stored: the deltas, widths, and sizes are those of the original layout, so compression is unchanged (identical bytes per row on the A-share 1-minute bars with DAY and MONTH partitions), whereas per-lane bases would add 128 bytes to every block.

Measured on one core of a Xeon Platinum 8260 (AVX-512) with 1024-value vectors in cache: decoding takes about 0.65 cycles per value for ALP, 0.5 for FOR, and 1.0 to 1.2 for the delta encodings, where unpacking and summing take about half each; on the 1-minute bars with MONTH partitions, timestamps decode at 4.0 G values/s and DECIMAL prices at 2.4, against 0.9 to 1.2 and 0.6 to 0.8 before these kernels. ALP encoding reaches 0.26 values per cycle with AVX-512 and about 0.15 with AVX2, where a chain of dependent floating-point operations per value limits it; integer encoding, whose statistics (choosing the encoding) are kernels too, reaches 0.23 to 0.28 values per cycle with AVX-512 on the 1-minute bars with MONTH partitions (0.04 to 0.06 before) and about 0.13 to 0.15 with AVX2. With DAY partitions, timestamps reach 0.23 and prices 0.20; volumes stay at 0.15, since their common divisor needs several greatest-common-divisor steps of a few hundred cycles per chunk however short the chunk is. On the M1 with NEON, integer encoding of the same MONTH data reaches 0.15 to 0.18 values per cycle, against 0.09 to 0.13 before.

### 4.3 Block Formation
1. The output of a flush or compaction is first sorted by `(series_id, ts)`.
2. A series with many rows is cut into blocks of 1024 rows, each holding only that series.
3. The tail of a series (fewer than 1024 rows) gets its own block if it has at least 128 rows; smaller tails are packed together with neighboring series.
4. Packed blocks carry an extra series_id column, RLE-encoded, which costs almost nothing.

This keeps most data in single-series blocks, where SMA pushdown applies, while tens of millions of sparse series do not produce tens of millions of tiny blocks. As compaction runs, each series accumulates longer runs and gradually moves into single-series blocks.

### 4.4 Segment File Format

**Conventions**
- Little-endian throughout.
- The file is organized in 4 KB-aligned pages that can be read with O_DIRECT straight into buffer pool frames without an extra copy. Alignment padding costs about 3% of space.
- A file is never modified after it is written. It takes effect only once it is recorded in the MANIFEST.

```
┌────────────────────────────── File header (page 0, 4096 B) ──────────────────────────────┐
│ 0   u8[8]  magic "SXDBSEG\0"          48  i64  min_ts                                    │
│ 8   u16    major                      56  i64  max_ts                                    │
│ 10  u16    minor                      64  u64  row_count                                 │
│ 12  u32    flags                      72  u32  block_count                               │
│ 16  u64    segment_id                 76  u16  column_count                              │
│ 24  u32    table_id                   78  u16  reserved                                  │
│ 28  u32    schema_version             80  u64  max_lsn (WAL position covered)            │
│ 32  i64    partition_lo  [lo, hi)     88  u32  reserved                                  │
│ 40  i64    partition_hi               92  u32  header_crc (bytes 0-91)                   │
├─────────── Data pages (4 KB aligned, normally <= 64 KB, one column per page) ────────────┤
│ 0   u32  magic "SXPG"                 16  u16  n_chunks                                  │
│ 4   u32  payload_len                  18  u16  flags (ZSTD / LZ4 second stage)           │
│ 8   u32  column_id (high 16 bits:     20  u32  crc32c (whole page, this                  │
│          array sub-column)                     field counted as 0)                       │
│ 12  u32  first_block                  24  u32  chunk_off[n_chunks]                       │
│ then: chunk, chunk, ... (one chunk = one column of one block; never spans pages)         │
├─────── Metadata region (4 KB aligned, paged, read on demand via the buffer pool) ────────┤
│ Column directory  32 B per column: column_id u32 | type u16 | flags u16 |                │
│                   page_count u32 | reserved u32 | page_index_off u64 | sma_off u64       │
│ Page index        16 B per page: file_offset u64 | length u32 | first_block u32          │
│ Block index       40 B per block: first_series u64 | last_series u64 | min_ts i64 |      │
│                   max_ts i64 | rows u16 | flags u16 (single-series bit) | reserved u32   │
│ SMA               64 B per column per block: min | max | sum (16 B) | count u32 |        │
│                   null_count u32 | first | last | reserved                               │
│ Sparse index      one 24 B entry per 64 KB of block index: first_block u32 |             │
│                   reserved u32 | first_series u64 | min_ts i64                           │
│ Series bitmap     every series_id in this segment (64-bit Roaring); used for the         │
│                   partition active-series bitmap                                         │
├───────────────────────────── Footer (last 64 B of the file) ─────────────────────────────┤
│ 0   meta_offset u64      8   meta_len u64        16  sparse_off u64                      │
│ 24  series_bm_off u64    32  sparse_len u32      36  series_bm_len u32                   │
│ 40  meta_crc u32         44  major u16           46  minor u16                           │
│ 48  reserved u8[8]       56  magic "SXDBEND\0"                                           │
└──────────────────────────────────────────────────────────────────────────────────────────┘
```

**Chunk** (one column of one block, at most 1024 rows):
```
u8 encoding | u8 flags (has NULL / all NULL / has unwritten) | u16 row_count | u32 data_len
[NULL bitmap: ceil(rows/64) x 8 bytes, present only if the chunk has NULLs]
[unwritten bitmap: ceil(rows/64) x 8 bytes, present only if the chunk has partial-column writes]
[encoding parameters + encoded data]
```

**Notes**
- When a page's flags mark second-stage compression, everything after `chunk_off` is compressed as a whole, and `chunk_off` holds offsets into the decompressed data.
- A chunk larger than 64 KB (long strings, large arrays) gets a page of its own, rounded up to a multiple of 4 KB.
- The **unwritten bitmap** marks "this row did not write this column", to tell it apart from "this row wrote NULL" (see §3.3).
  - Unwritten rows hold a NULL placeholder in the encoded data. On read, their values come from older data.
  - When every row in a chunk wrote the column (the common case), there is no such bitmap and no extra cost.
- SMA `sum`: 128-bit integer for integers and DECIMAL; double with a compensation term for floating point. `first`/`last` are the first and last non-NULL values in the block. Non-numeric columns fill only `count` and `null_count`. Unwritten values count as NULL.
- Version rule: a reader refuses an unknown major version. A newer minor version is readable as long as all flags are understood. The overall upgrade strategy is in §14.
- Read path: footer → sparse index → block index page → the column's page index → data page → chunk. Even a very large segment never has to be loaded whole.

### 4.5 MANIFEST
- A per-table, append-only version log. Record format: `[len u32][crc32c u32][type u8][payload]`.
- Record types: `SNAPSHOT`, `ADD_SEGMENT`, `REMOVE_SEGMENT`, `FLUSHED_LSN`, `TOMBSTONE`, `SCHEMA_VERSION`; for the series index `ADD_INDEX_SEGMENT`, `REMOVE_INDEX_SEGMENT`, `SERIES_TOMBSTONE`; for continuous aggregates `VIEW_INVALIDATION`, `VIEW_WATERMARK` (see §8.7).
- All changes from one flush or compaction go into a single record, so the commit is atomic even across partitions.
- When the log grows too long, a full snapshot is written to `MANIFEST-<n+1>` and `CURRENT` is switched atomically (write temp file → fsync → rename → fsync directory).
- At startup, segment files not referenced by the MANIFEST are treated as crash leftovers and removed.
- The CATALOG uses the same format and commit procedure.

### 4.6 WAL
- One WAL per instance, 64 MB per segment.
- Record format: `[len u32][crc32c u32][lsn u64][commit_time i64][type u8][flags u8][payload]`. LSNs increase monotonically across the instance; `commit_time` is used for point-in-time recovery.
- `ROW_BATCH` payloads larger than 4 KB are compressed with LZ4 (flags bit 0), which keeps WAL bandwidth well below disk bandwidth at high write rates (see §7). The CRC covers the stored bytes, so a damaged record is detected before decompression.
- Record types:
  - `ROW_BATCH`: a columnar batch already resolved to series_ids, with the list of written columns. Batches whose rows have different field sets (such as line protocol) also carry a per-row written mask. Definitions of series created by this batch (series_id and tag values) go in the same record, before the rows.
  - `DELETE`: a tombstone.
  - `BULK_COMMIT`: references to segments produced by a bulk import.
- Group commit. fsync policy: `always`, `1s` (default), or `off`.
- A WAL segment can be deleted once every table's `FLUSHED_LSN` has passed it and, if WAL archiving is enabled, once it has been archived.
- **Crash recovery**:
  1. Read each table's MANIFEST to get its `FLUSHED_LSN`.
  2. Replay the WAL from the smallest `FLUSHED_LSN`; each table skips records it has already flushed.
  3. Truncate a partially written record at the end of the WAL.

  Because writes follow "later overwrites earlier", replay is idempotent.

### 4.7 Write Path
1. An IO thread parses the request (line protocol, `INSERT`, or Arrow), looks up each `series_id` in the series index, and creates new series as needed. It then splits the batch by shard.
2. The WAL thread group-commits the batch, assigns an LSN, and hands the sub-batches to the shards in LSN order. Only pointers are passed; data is not copied.
3. Each shard thread applies its rows to the MemTable shard it owns exclusively, without locks, and updates the Last Cache.
4. The client is acknowledged only after every involved shard has applied its rows, so an acknowledged write is immediately readable.

### 4.8 MemTable
- Split into `write_shards` shards by `series_id` hash; each shard is owned by one thread.
- Each series holds a chain of buffers whose capacity starts at 8 rows and doubles up to 1024 rows. A sparse series costs a few dozen bytes; a dense series is as efficient as a preallocated buffer.
- Values are stored raw, uncompressed, for the fastest possible writes.
- Each row carries its batch LSN, run-length encoded, which costs almost nothing for dense series. Queries use it for snapshot filtering.
- Each row records which columns it wrote. When a whole batch writes the same columns, this is recorded once.
- Consecutive writes to the same series and timestamp (for example line protocol sending fields on separate lines) are merged column by column in place in the MemTable, so most data reaches disk as complete rows.
- Series with out-of-order writes are flagged and sorted at flush or read time.
- Total size is bounded by `memtable_limit`. At the limit, the current MemTable is frozen and handed to a background flush; if flushing cannot keep up, writes are throttled with backpressure.

### 4.9 Flush
A frozen MemTable is written to disk in these steps:
1. Sort by (series_id, ts). Multiple writes to the same key are merged column by column: each column takes its most recent written value; writes that did not write that column do not participate.
2. Split by partition, encode, write segment files, fsync.
3. Write the in-memory part of the series index as an index segment and fsync it.
4. Write one MANIFEST record and fsync it. The record contains the new segments, index segments, view invalidation ranges, and the advanced `FLUSHED_LSN`.
5. Release the MemTable memory.

### 4.10 Read Path
1. At query start, pin a snapshot: the current Version (each table's segment set, reference counted), the catalog version, and the visible LSN.
2. Prune partitions by time range. Use TAG conditions to get a candidate series set from the series index, then intersect it with each partition's active-series bitmap (see §5.1).
3. For each segment: sparse index → block index → SMA check (use SMA directly where possible, without decoding) → fetch pages through the buffer pool → evaluate filters, on encoded data where possible (see §5.3) → decode the remaining columns vector by vector, only for vectors with selected rows.
4. Merge data from multiple segments and the MemTable by (series, ts). For each key, each column takes its most recent written value (skipping unwritten ones); then tombstones are filtered out. Only the columns the query needs are merged.
5. **Fast path**: if a partition has a single segment and the MemTable holds no data for that partition (typical for cold data), skip merging and scan directly.

### 4.11 Compaction
- **Regular merges**: segments within a partition are merged by size tiers, deduplicating and applying tombstones.
- **Only segments adjacent in write order are merged**, so "later overwrites earlier" can always be decided.
- When a merge includes the partition's oldest segment, no older data exists, so remaining unwritten values become NULL and the unwritten bitmaps are dropped.
- **Cold partitions**: a partition with no writes for two partition intervals is considered cold and gets a full merge, recompressed if the table enables `cold_compression`. A cold partition usually ends up as a single file.
- Runs in the background with limits on IO bandwidth and threads, so it does not disturb foreground writes and queries.

### 4.12 Bulk Import
- `COPY ... FROM` and Arrow writes above a threshold (64 MB by default) take a dedicated path: they bypass the WAL and the MemTable, are sorted in batches, and are written directly as segments.
- The whole import is committed atomically with one MANIFEST record at the end; the imported data counts as the newest write.
- A `BULK_COMMIT` record referencing these segments is written to the WAL. When WAL archiving is enabled, the segments are archived too, so point-in-time recovery stays complete.

### 4.13 Last Cache
- Caches the latest row of each series; enabled with the table option `WITH (last_cache = on)`.
- Updated by shard threads on write, merging column by column in the same way.
- Lives in the buffer pool and can be evicted. On a miss, the latest partition is read.
- The optimizer recognizes `DISTINCT ON (...) ... ORDER BY ts DESC` and per-series `last()` and answers them from the cache.

---

## 5. Indexes

### 5.1 Series Index

One per table. It must work from one series to hundreds of millions without requiring the whole index to be in memory.

**Data structures**
- **TAG value dictionaries**: one per TAG column, mapping strings to integer `value_id`s. Append-only.
- **Series key**: a series' tag combination encoded as a fixed-length tuple of `value_id`s, for example 12 bytes for 3 TAGs. Comparisons and hashing are fixed-length integer operations. GROUP BY on tags also groups by `value_id` and converts back to strings only when producing output.
- **Three mappings**:

| Mapping | Purpose | Structure |
|---|---|---|
| Forward: series key → series_id | Find or create series on write | Sorted array + sparse index + Bloom filter |
| Reverse: series_id → series key | Output tags, group by tags | Fixed-width array indexed directly by series_id |
| Inverted: (TAG column, value_id) → series bitmap | Filter by tags | 64-bit Roaring Bitmap (in-house) |

**LSM organization**
- New series go into an in-memory part first. When the table flushes, the in-memory part is written as an immutable index segment (`.sxi`) and committed in the same MANIFEST record.
- Because series_ids are assigned in creation order, each index segment covers exactly one contiguous series_id range `[a, b)`. This gives three benefits:
  - The reverse mapping is a fixed-width array for the range, indexed directly by `series_id - a`.
  - When index segments merge, their inverted bitmaps cover disjoint ranges, so merging is essentially concatenation.
  - The forward mapping needs a single merge sort.
- Index segments merge by tiers and stay at around a dozen or fewer.
- Startup only opens the files without loading them, so startup time does not depend on series count.
- All pages go through the buffer pool and count against `memory_limit`.
- The byte-level format of index segments is finalized in M1.

**Series lookup on write**
1. Check the hot cache first: raw tag string (for example `trades,symbol=600519.SH` in line protocol) → series_id. Almost all writes hit here, without even touching the dictionaries. The hot cache is evictable and counts against the memory limit.
2. On a miss, check the in-memory part and then each index segment, using Bloom filters to skip segments that cannot contain the key.
3. If the series still is not found, create it:
   - Take a striped lock keyed by the series key hash, then look up again, so two threads cannot create the same series.
   - Allocate the series_id from an atomic counter.
   - The series definition is written in the same WAL record as the rows (see §4.6). Crash recovery rebuilds it with the recorded series_id; ids are never reassigned.

**Resolving series sets for queries**
- `=`, `IN`: take the corresponding bitmaps directly.
- `!=`, `NOT IN`: subtract the corresponding bitmaps from the full set.
- `LIKE`, regular expressions: scan the column's TAG value dictionary for matching values, then combine their bitmaps. The number of values is usually far smaller than the number of series.
- AND, OR: bitmap intersection and union.
- `SHOW TAG VALUES` and Grafana variable queries read the TAG value dictionary directly; with a time range, results are filtered by the active-series bitmaps below.

**Partition active-series bitmaps**
- Every segment's metadata stores all series_ids it contains (see §4.4). A partition's active bitmap is the union of its segments' bitmaps plus the series in the MemTable, computed on demand and cached.
- At query time, the series set from tag conditions is intersected with the active bitmaps of the partitions in the time range.
- This handles high series churn. For example, option contracts expire daily and containers are recreated constantly: `underlying='510050'` may match hundreds of thousands of historical contracts, while only a few hundred have data today. With active bitmaps, queries do not probe segments for series that have long had no data.

**Automatic cleanup of dead series**
- When TTL or `DROP PARTITION` removes a partition, compute the series that appeared only in removed partitions: active bitmap of the removed partition − union of the other partitions' active bitmaps − series in the MemTable.
- These series are first recorded in a deletion bitmap (`SERIES_TOMBSTONE`) and excluded from queries.
- They are physically removed from the forward and inverted mappings when index segments merge. TAG values no longer referenced are removed too.
- series_ids are never reused. The 64-bit space cannot be exhausted at any churn rate.
- All of this is automatic and needs no configuration.

### 5.2 Time Index
- Partition directory names alone prune irrelevant partitions.
- Segment headers record min/max ts.
- The block index records min/max ts per block and is located by binary search through the sparse index.

### 5.3 Value Filtering
- Block-level min/max skip whole blocks. For example, with `WHERE usage > 90`, blocks whose maximum is below 90 are skipped.
- **Filtering on encoded data**: in the blocks that remain, comparisons with constants on integer, `DECIMAL`, and `TIMESTAMP` columns are evaluated without rebuilding the values where the encoding allows it:
  - FOR_BITPACK: the chunk's base and multiplier are folded into the constant, so `x > c` becomes a comparison on the unpacked offsets;
  - CONSTANT and RLE: the comparison runs once per run;
  - other encodings (and ALP for floating point) are decoded first, which is cheap.

  The result is a selection vector per 1024-row vector.
- **Late materialization**: the other columns the query needs are decoded only for vectors that have at least one selected row. Selective filters therefore decode little more than the filter columns.

### 5.4 SMA Pushdown
- When a block falls entirely inside the query's time window and filters, the block header's min/max/sum/count/first/last are used directly, without decoding.
- Aggregations grouped by series use SMA only on blocks flagged as single-series.

---

## 6. Memory Management

### 6.1 Configuration

```ini
memory_limit       = 8G     # total limit; fixed values only (K/M/G/T, base 1024)
memtable_limit     = 2G     # default: min(memory_limit/4, 2G)
query_memory_limit = 4G     # per-query limit; default: memory_limit/2
```

- **Defaults when unset**: 1G for the standalone server; 256M for the embedded library (set by the host program in `sxdb_open()`). Minimum 32M.
- **Startup check**: if `memory_limit` exceeds physical memory or the container's cgroup limit, startup is refused with an explanation.
- **Runtime changes**: `ALTER SYSTEM SET memory_limit = '16G';`. Lowering the limit evicts cache immediately and returns memory to the OS with `madvise`.

### 6.2 Unified Memory Manager

Every memory consumer allocates through the memory manager and shares one budget:

| Consumer | How memory is released | Policy |
|---|---|---|
| Buffer pool (block cache) | Evictable | Takes all remaining space; this is what keeps memory fully used |
| MemTable | Released after flush | Bounded by `memtable_limit`; at the limit, writes get backpressure |
| Query working memory | Released when the query ends | Reserved on demand. First evicts cache to make room; if still short, waits in a queue and fails on timeout |
| Series index, Last Cache | Evictable | Pages live in the buffer pool |
| Connection buffers | Released when the connection closes | Grow on demand, with a cap |
| Thread stacks, code, small objects | — | Not tracked byte by byte. Estimated from thread counts at startup and deducted from the budget up front |

- **Large allocations** use fixed size classes: 64 KB frames, and power-of-two classes above that. They come from anonymous mmap; accounting is a single atomic add. Small objects are allocated from arenas that are released as a whole when a query ends.
- **Under memory pressure**, in order: evict cache, flush the MemTable early, make queries wait. Memory managed by sxdb never exceeds the limit.
- **Eviction** uses CLOCK, with higher priority for metadata pages (block index, page index). Large scans covering more than 1/4 of the buffer pool use only a small ring buffer, so they do not push hot data out (the same approach as PostgreSQL).
- **Monitoring**: the `sx_memory` system table shows usage per consumer in real time.

### 6.3 IO
- Files are read with pread into a self-managed cache, not with mmap. With mmap, cache size and eviction are decided by the kernel and cannot be kept within the configured limit; there are also TLB shootdowns and IO errors that surface as SIGBUS (see the CIDR 2022 paper *Are You Sure You Want to Use MMAP in Your Database Management System?*).
- `io_mode = direct`: the default for the standalone server. O_DIRECT on Linux, falling back to regular reads when the file system does not support it; F_NOCACHE on macOS. Data is cached once, in sxdb, not duplicated in the OS page cache. **Note: in this mode `memory_limit` is essentially the entire cache, so production systems should size it to the machine.**
- `io_mode = buffered`: the default for the embedded library, also suitable when sharing a machine with other programs.
- The cache holds lightweight-encoded data, not decoded data: the same memory holds several times more data, and decoding is fast enough to do on every use. For ZSTD-compressed cold data, the cache holds the layer below ZSTD.
- Sequential scans prefetch upcoming pages, first with pread on a thread pool; io_uring may be used on Linux later.
- In `direct` mode, segment files are written with O_DIRECT. The WAL uses regular writes plus `fdatasync`.

---

## 7. Threads and Concurrency

```
 connections ─► IO threads xN (epoll/kqueue; SO_REUSEPORT on Linux spreads connections)
                  │ protocol parsing, line protocol parsing, series_id lookup/creation,
                  │ splitting by shard, SQL parsing and planning
                  │
        writes ───┤                         queries ─► query scheduler ─► workers xM (morsels)
                  ▼                                                          │
            WAL thread x1: group commit, assigns LSNs                         │
                  │ hands sub-batches to shards in LSN order (pointers only)  │ snapshot reads
                  ▼                                                          │
            shard threads xS: each owns one MemTable shard, lock-free writes ◄┘
                  │ frozen MemTables are handed off
                  ▼
            background threads xB: flush / compaction / WAL archiving / TTL / view refresh
                                   (low priority, IO throttled)
```

| Setting | Default | Notes |
|---|---|---|
| `io_threads` | max(2, cores/8) | Network IO plus CPU-heavy protocol parsing. Parsing is usually the first bottleneck of the write path, so this scales with the machine |
| `write_shards` | max(1, cores/4) | Number of MemTable shards, each owned by one thread |
| `query_threads` | cores | One pool shared by all queries |
| `background_threads` | 2 | Flush, compaction, and other background tasks |

**Writes**
- A single WAL thread establishes the global order and hands data to shards in LSN order. Every shard receives its data already ordered, without locks.
- One WAL is enough: 5 million points per second is roughly 50 MB/s of sequential writes, far below disk bandwidth. At higher rates (10 million rows per second of 8 columns is about 800 MB/s uncompressed), LZ4 compression of `ROW_BATCH` payloads (see §4.6) keeps it within reach of one thread and one disk; the compression ratio on real batches is measured in M1. A single WAL also keeps point-in-time recovery simple.

**Queries**
- Morsel-driven parallelism (Leis et al., SIGMOD 2014): a query is split into small tasks of "partition × block range" that workers pick up; partial results are combined at the end.
- Queries are scheduled round-robin, so one large query cannot occupy the whole pool. Each query can have a parallelism cap and a priority; backups and exports run at low priority.
- Workers encode results directly into PG or Arrow format; IO threads only send them. When a client reads slowly, the query pauses instead of piling results up in memory.

**NUMA** (Linux machines with more than one memory node; macOS machines have one)
- At startup the node topology is read from `/sys/devices/system/node`. Query workers and shard threads are split evenly across nodes and pinned to them.
- Buffer pool frames belong to a node: a frame is filled by a thread on the node that reads it, so its memory is placed there (first-touch placement; `mbind(2)` is called directly where needed, with no libnuma dependency). The memory budget stays global.
- Morsels are dispatched first to workers on the node that holds their pages; idle workers steal from other nodes, as in the morsel-driven design.
- `numa = auto | off` (default `auto`): `off` disables pinning and placement, for example to compare in benchmarks.

**Isolation**
- **Writes**: a batch becomes visible entirely or not at all.
- **Reads**: statement-level snapshots. A query takes a visible LSN at start and does not see later commits. The visible LSN advances only after all smaller batches have been applied to the MemTable.
- **Transactions**: multi-statement transactions are not supported. For PG driver compatibility, `BEGIN` and `COMMIT` are accepted but do nothing; `ROLLBACK` returns an error if the "transaction" performed writes, stating clearly that they cannot be rolled back.
- **DDL**: the catalog is versioned and each query pins one version. `ADD COLUMN` creates a new schema version; old segments lack the column, which reads as NULL. A dropped table is physically removed once no query references it. DDL returns only after it is written to the CATALOG and fsynced.
- **Memory reclamation for lock-free structures**: epoch-based reclamation (EBR).

**Embedded mode**: the library starts the same background threads, with thread counts set in `sxdb_open()`. The API is thread-safe. Queries can run on the calling thread or on the library's thread pool.

---

## 8. SQL

### 8.1 Principles
- **PostgreSQL syntax is the reference.** Anything PostgreSQL already has is written the PostgreSQL way. Time-series extensions reuse TimescaleDB function names where possible, so Grafana, drivers, and SQL tools recognize them.
- **Parser**: hand-written recursive descent with Pratt parsing for expressions; no yacc/bison.
- **Optimizer**: rule-based, including:
  1. Partition pruning by time range
  2. Series sets from TAG predicates via the inverted index
  3. Projection pushdown: read only the columns the query uses
  4. Predicate pushdown: skip whole blocks by block-level min/max
  5. Aggregate pushdown to SMA
  6. `LIMIT` and `ORDER BY ts` pushdown
  7. Streaming aggregation when grouping by series or time (data is already ordered), with no hash table
- **Executor**: vectorized Volcano model; each `next()` returns 1024 rows, and vectors use the Arrow memory layout directly. Operators are type-specialized tight loops (generated with macros), plus AVX2, AVX-512, and NEON kernels chosen once at startup by CPU features, with the same dispatch mechanism as the decoders (see §4.2).

### 8.2 DDL
- `CREATE/DROP DATABASE`. The database in the connection parameters is the default; `db.table` reaches across databases.
- `CREATE TABLE [IF NOT EXISTS] t (...) [PARTITION BY HOUR|DAY|WEEK|MONTH|YEAR] [TTL 90d] [WITH (last_cache = on, cold_compression = 'zstd', ...)]`
- `ALTER TABLE`: `ADD/DROP/RENAME COLUMN`, `SET TTL`, `SET (...)`, `DROP PARTITION WHERE ...`. Changing a column's type is not supported.
- `DROP TABLE`, `TRUNCATE TABLE`
- `CREATE/ALTER/DROP USER`, `GRANT`, `REVOKE`
- `CREATE / DROP MATERIALIZED VIEW`, `REFRESH MATERIALIZED VIEW` (continuous aggregates, see §8.7)

### 8.3 DML
- `INSERT ... VALUES` (multi-row), `INSERT ... SELECT`. With a column list, only the listed columns are overwritten (see §3.3).
- `DELETE`: time range and TAG conditions only (see §3.4).
- `COPY t FROM 'file' | STDIN`, `COPY (SELECT ...) TO 'file' | STDOUT`, in csv, binary, or arrow format.

### 8.4 Queries
- `SELECT [DISTINCT | DISTINCT ON (...)] ... FROM ... WHERE ... GROUP BY ... HAVING ... ORDER BY ... LIMIT/OFFSET`
- Non-recursive `WITH` CTEs; subqueries in FROM, scalar subqueries, `IN (subquery)`; `UNION [ALL]`
- Joins:
  - `INNER/LEFT JOIN`: hash join, bounded by the memory limit, intended for dimension tables
  - `ASOF [LEFT] JOIN`
- Window functions: aggregates with `OVER`, `lag/lead`, `first_value/last_value`, `row_number/rank`. Frames support `ROWS` and time-based `RANGE BETWEEN INTERVAL '5 minutes' PRECEDING AND CURRENT ROW`.
- `EXPLAIN [ANALYZE]`

```sql
-- ticks -> 1-minute bars, aligned to the exchange time zone
SELECT time_bucket('1m', ts, 'Asia/Shanghai') AS bar, symbol,
       first(price) AS open,  max(price)  AS high,
       min(price)   AS low,   last(price) AS close,
       sum(volume)  AS volume,
       sum(price * volume) / sum(volume) AS vwap
FROM trades
WHERE symbol IN ('600519.SH', '000001.SZ')
  AND ts >= '2026-10-16 09:30:00+08' AND ts < '2026-10-16 15:00:00+08'
GROUP BY bar, symbol
ORDER BY bar;

-- 1-minute close prices with gaps filled
SELECT time_bucket_gapfill('1m', ts, '2026-10-16 09:30+08', '2026-10-16 15:00+08') AS bar,
       locf(last(price)) AS close
FROM trades
WHERE symbol = '600519.SH'
  AND ts >= '2026-10-16 09:30+08' AND ts < '2026-10-16 15:00+08'
GROUP BY bar;

-- each trade with the most recent quote before it
SELECT t.ts, t.symbol, t.price, q.bid, q.ask
FROM trades t ASOF JOIN quotes q
  ON t.symbol = q.symbol AND t.ts >= q.ts;

-- 20-day moving average
SELECT ts, symbol, close,
       avg(close) OVER (PARTITION BY symbol ORDER BY ts
                        ROWS BETWEEN 19 PRECEDING AND CURRENT ROW) AS ma20
FROM daily_bars;

-- latest trade per symbol (served by the Last Cache)
SELECT DISTINCT ON (symbol) symbol, ts, price FROM trades ORDER BY symbol, ts DESC;

-- order book: best bid, best ask, and bid volume over the top five levels
SELECT ts, bid_px[1] AS best_bid, ask_px[1] AS best_ask,
       array_sum(bid_vol[1:5]) AS bid_depth5
FROM l2_snapshot WHERE symbol = '600519.SH';
```

### 8.5 Functions

| Category | Functions |
|---|---|
| Aggregate | `count sum avg min max first last stddev variance spread corr covar`; exact `percentile_cont/disc`; approximate `approx_percentile` (t-digest) and `approx_count_distinct` (HyperLogLog) |
| Time series | `time_bucket(width, ts [, timezone] [, origin])`, `time_bucket_gapfill`, `locf`, `interpolate`, `delta`, `rate`, `twa` (time-weighted average) |
| Array | subscripts, slices, `cardinality`, `array_sum/avg/min/max`, `unnest` |
| Time | `now`, `date_trunc`, `extract`, `AT TIME ZONE`, `INTERVAL` arithmetic, `to_timestamp` |
| Scalar | common math functions; `length/lower/upper/substr/concat`; `LIKE`; regular expressions (libc regex); `CASE`, `coalesce`, `nullif`, `greatest/least`, `CAST` |

`time_bucket` supports time zones (handling daylight saving time correctly) and an origin argument (for example, bars aligned to start at 09:30). Time zone data comes from the system's `/usr/share/zoneinfo`.

### 8.6 Operations and Metadata
- Operational commands: `BACKUP`, `CHECKPOINT`, `OPTIMIZE TABLE` (force compaction), `ALTER SYSTEM SET`, `SET timezone = ...`
- Query cancellation: `pg_cancel_backend()`, plus PG protocol cancel requests
- System tables: `sx_status`, `sx_databases`, `sx_storage`, `sx_tables`, `sx_series`, `sx_partitions`, `sx_segments`, `sx_memory`, `sx_queries`, `sx_views` (the first few are described in §12, `sx_views` in §8.7)
- A minimal `information_schema` and `pg_catalog` for Grafana and drivers

### 8.7 Continuous Aggregates (Downsampling)

Continuous aggregates are materialized views refreshed by recomputing invalidated buckets (the TimescaleDB approach). Two other common approaches are not used:
- **In-place downsampling**: one table would mix data at different resolutions, making results hard to interpret.
- **Incrementally merging aggregate states**: sxdb supports overwrites and DELETE; once old values are overwritten, aggregates such as sum become wrong.

**Usage**

```sql
-- keep raw ticks for 30 days
ALTER TABLE trades SET TTL 30d;

-- 1-minute bars, kept for 10 years
CREATE MATERIALIZED VIEW bars_1m
WITH (refresh_interval = '1m', refresh_lag = '10s', ttl = '10y')
AS SELECT time_bucket('1m', ts, 'Asia/Shanghai') AS ts, symbol,
          first(price) AS open,  max(price)  AS high,
          min(price)   AS low,   last(price) AS close,
          sum(volume)  AS volume
   FROM trades
   GROUP BY 1, symbol;

-- daily bars aggregated from 1-minute bars (cascading), kept forever
CREATE MATERIALIZED VIEW bars_1d
WITH (refresh_interval = '1h')
AS SELECT time_bucket('1d', ts, 'Asia/Shanghai') AS ts, symbol,
          first(open) AS open, max(high) AS high,
          min(low)    AS low,  last(close) AS close,
          sum(volume) AS volume
   FROM bars_1m
   GROUP BY 1, symbol;

-- recompute a time range manually
REFRESH MATERIALIZED VIEW bars_1m FROM '2026-10-01' TO '2026-10-08';
```

**A view is an ordinary table**
- The time bucket column is its time column, the GROUP BY tag columns are its TAGs, and the aggregates are its value columns.
- Querying a view is exactly like querying a table. A view has its own TTL, partitions, and backups.
- Views can cascade (a view built on a view). Functions such as avg cannot cascade (an average of averages is not the overall average); store sum and count instead.

**Refresh**
- Each view has a **watermark**: all time buckets before it are complete.
- **Normal in-order writes** land after the watermark. Once a bucket has ended and `refresh_lag` has passed, the background computes the bucket and advances the watermark.
- **Late writes, overwrites, and DELETEs** land before the watermark. The affected (series, time bucket) pairs are recorded in an invalidation list, adjacent ranges are coalesced, and they are recomputed on the next refresh.
  - The only cost on the write hot path is one comparison per batch: is the batch's minimum timestamp earlier than the watermark?
- If a recomputed bucket no longer has source data (it was deleted), the corresponding view rows are deleted too.
- **TTL and `DROP PARTITION` do not affect views**: when raw data expires, the view's results remain. This is what makes "raw data for 30 days, bars for 10 years" work.
- When a view is created, existing history is backfilled in the background at low priority; progress is shown in `sx_views`.
- Refreshes run on background threads and are bounded by the memory limit.

**Durability**
- Invalidation ranges are first kept in memory. When the source table flushes, they are written to the source table's MANIFEST as `VIEW_INVALIDATION` records, committed together with the data.
- After a crash before the flush, WAL replay regenerates the invalidation ranges.
- Refresh results are written into the view table through the normal write path. Once acknowledged, `VIEW_WATERMARK` is advanced in the view's MANIFEST and the processed invalidations are marked done.
- A crash between these steps only causes the same buckets to be recomputed. Because writes follow "later overwrites earlier", the result is still correct.

**Restrictions**
- A view is based on a single table; GROUP BY must include a time bucket; joins and window functions are not allowed.
- Queries are not rewritten to use views automatically; query the view explicitly. Automatic routing may come later.

### 8.8 Explicitly Unsupported
Multi-statement transactions, `UPDATE`, foreign keys, triggers, stored procedures, recursive CTEs, and non-equi joins other than ASOF.

---

## 9. Interfaces

### 9.1 PostgreSQL Wire Protocol v3
- Simple and extended query protocols, binary result format, `COPY FROM/TO STDIN` (csv/text), cancel requests.
- Authentication with SCRAM-SHA-256.
- Text-format results format floating-point values with an in-house shortest round-trip algorithm (Ryu-style), not `printf`, whose cost often dominates large result sets. Numbers in text input are parsed with a fast exact algorithm (Eisel–Lemire-style). The binary format and Arrow need no formatting at all and are recommended for large results.
- Later: `COPY ... TO STDOUT (FORMAT binary)` and a fuller `pg_catalog`, so `adbc-driver-postgresql` and ConnectorX can connect directly.

### 9.2 InfluxDB Line Protocol
- Over TCP and HTTP. HTTP provides compatible `/write` (v1) and `/api/v2/write` endpoints, so Telegraf works out of the box.
- Token authentication.
- Line protocol has no arrays; array columns are written through the PG protocol, Arrow, or the C API.
- Parsing is the most CPU-heavy step of the write path. Lines are parsed straight into columnar batches with no per-row allocation; numbers and timestamps use the fast parsers from §9.1. Bulk loads should use Arrow or `COPY`, which skip text parsing.

### 9.3 HTTP API
| Endpoint | Description |
|---|---|
| `POST /api/v1/query` | The body is SQL. The result format follows the `Accept` header: JSON, CSV, or an Arrow IPC stream (`application/vnd.apache.arrow.stream`) |
| `POST /api/v1/write?table=...` | The body is an Arrow IPC stream, line protocol text, or CSV |
| `GET /api/v1/status` | Service status and metrics as JSON (see §12) |
| `GET /metrics` | Self-monitoring metrics in Prometheus format (see §12) |
| `GET /health` | Health check; no authentication (see §12) |

### 9.4 Arrow
- Executor vectors are already in the Arrow memory layout, so output needs no conversion; buffers go straight to the network.
- Results stream in batches of about 64K rows. Exporting hundreds of millions of rows does not accumulate memory on the server.
- Arrow's built-in LZ4/ZSTD buffer compression can be enabled.
- Type mapping:

| sxdb | Arrow |
|---|---|
| `TIMESTAMP` | `timestamp[ns, UTC]` |
| `TAG` / `SYMBOL` | dictionary type (Categorical in pandas and polars) |
| `DECIMAL` | `decimal128` |
| `T[N]` | FixedSizeList |
| `T[]` | List |

### 9.5 C API (sketch; finalized in M1)

The public API uses only opaque handles and setter functions; struct layouts are not exposed (see §14.4).

```c
sxdb_options* opts = sxdb_options_new();
sxdb_options_set_memory_limit(opts, 256u << 20);

sxdb* db = NULL;
int rc = sxdb_open("/var/lib/myapp/tsdb", opts, &db);
sxdb_options_free(opts);
if (rc != SXDB_OK) {
    /* ... */
}

sxdb_exec(db, "CREATE TABLE trades (...)", NULL);

sxdb_appender* ap = NULL;                   /* columnar bulk append, the fastest write path */
sxdb_appender_open(db, "trades", &ap);
/* ... */
sxdb_appender_close(ap);

struct ArrowArrayStream out;                /* results via the Arrow C Data Interface */
sxdb_query_arrow(db, "SELECT ...", &out);

sxdb_close(db);
```

### 9.6 Python Client (MIT)
A thin package, `sxdb`, depending only on pyarrow and talking to the server over HTTP + Arrow.

```python
import sxdb

db  = sxdb.connect("http://127.0.0.1:8440", token="...")
tbl = db.query_arrow("SELECT ...")
df  = db.query_polars("SELECT ...")
pdf = db.query_pandas("SELECT ...")     # ArrowDtype backend by default, so decimals do not become Python objects
db.write("trades", df)                  # pandas, polars, or pyarrow
```

The package is optional: `requests` plus `pyarrow.ipc.open_stream` is enough to read results.

### 9.7 Command-Line Client: sxql
An in-house interactive SQL client whose usage follows psql as closely as possible, to keep the learning curve flat. Operational tasks such as init, backup, restore, verify, and dump remain in the `sxdb` command.

**Connections**
- **Network mode**: connects to the server over the PG wire protocol, for example `sxql -h 127.0.0.1 -p 5440 -U admin -d iot`.
- **Local mode**: `sxql --data-dir /var/lib/myapp/tsdb` opens a data directory directly through libsxdb, for embedded deployments and offline inspection. If another process (such as a running server) holds the directory lock, it refuses to open.

**Interaction**
- Multi-line input terminated by `;`, with history.
- Tab completion for keywords and for database, table, column, and function names.
- Line editing is based on linenoise (BSD-2-Clause, vendored), extended with UTF-8 and East Asian wide-character support. GNU readline is not used: it is GPLv3, and linking it would force the whole program to GPLv3.
- Results are aligned by display width, with CJK characters counted as double width. Output longer than a screen goes through a pager (`$PAGER`, default `less -S`).
- Large results are displayed as a stream; the client never reads the whole result into memory.
- Ctrl+C cancels the running query (via a PG protocol cancel request) without exiting.

**Meta-commands** (matching psql)

| Command | Purpose |
|---|---|
| `\l` | List databases |
| `\c <db>` | Switch database |
| `\dt` | List tables |
| `\d <table>` | Describe a table, including TAGs, partitioning, TTL |
| `\x` | Toggle expanded display |
| `\timing` | Show execution time |
| `\o <file>` | Redirect output to a file |
| `\i <file>` | Run a script file |
| `\copy` | Read or write client-side files, transferred via `COPY ... FROM STDIN / TO STDOUT` |
| `\e` | Edit the current statement in `$EDITOR` |
| `\status` | Show service status, uptime, memory and storage usage (see §12) |
| `\?`, `\q` | Help, quit |

**Script mode**
- `sxql -c "SELECT ..."`, `sxql -f script.sql`, or input from stdin.
- `--format table|csv|json` selects the output format.
- Non-zero exit code on error, for use in scripts.

**Security**
- Passwords are entered interactively without echo. Passwords on the command line are not supported, because arguments show up in the process list.
- A password file `~/.sxqlpass` in the same format as psql's `.pgpass` is supported; it is refused if its permissions are wider than 0600.
- The history file `~/.sxql_history` has permissions 0600. Statements containing `PASSWORD` (such as `CREATE USER ... PASSWORD`) are not written to history.
- When built with Mbed TLS, TLS options match the server's.

---

## 10. Backup and Restore

Segment files never change after they are written, so backups are cheap and do not affect writes or queries. Backup IO is throttled and runs at low priority.

| Method | Usage | Mechanism |
|---|---|---|
| Online full | `BACKUP DATABASE iot TO '/backup/full_1016';` | Force a flush and pin the current Version; hard-link files on the same file system, or copy and verify across file systems; finally write a backup manifest (size and CRC of each file, plus the backup's LSN) |
| Incremental | `... INCREMENTAL FROM '/backup/full_1015'` | Compare with the previous backup manifest and copy only new segments (segment_ids are never reused); record removed segments |
| WAL archiving | Set `wal_archive_dir` | Each full WAL segment is archived by built-in code, along with segments from bulk imports. **No shell commands are executed.** Old archives are cleaned up by age and size (below) |
| Point-in-time recovery | `sxdb restore --from <full> [--incremental <dir> ...] --until '2026-10-16 12:00:00' --to <data_dir>` | Offline: restore the full and incremental backups, then replay archived WAL by `commit_time` up to the given moment. `--until-lsn` is also supported |
| Verification | `sxdb verify <backup or data directory>` | Check every page CRC, the MANIFEST, and the backup manifest |
| Logical export | `COPY ... TO`, `sxdb dump` | For migrating across versions |

The `sxdb backup` command can be scheduled with cron. Backup directories are 0700 and files are 0600.

Backup targets must be local file systems, including network storage mounted locally (NFS and similar). Backing up directly to object storage and backup encryption are not supported; for off-site or encrypted storage, process the backup directory with external tools.

### WAL Archive Retention

```ini
wal_archive_retention_time = 7d     # keep only the last 7 days
wal_archive_retention_size = 10G    # keep the archive under 10G
```

**Rules**
- Whichever limit is reached first, the oldest segments are deleted.
- Age is measured by the `commit_time` of the last record in each segment, so the full last 7 days of WAL are always kept.
- Size includes WAL segments and the segment files archived with bulk imports. Those segment files are deleted together with the WAL segment that references them.

**Values**
- 0 means no limit for that dimension.
- When archiving is enabled but neither value is set, the defaults are 7d and 10G.
- `wal_archive_retention_size` must be at least two WAL segments (128M), otherwise startup is refused.
- Both can be set in the configuration file or changed at runtime with `ALTER SYSTEM SET`; changes take effect immediately.

**Execution**
- The WAL archiver thread cleans up after each newly archived segment and checks the age limit once a minute.
- Only archive files written by sxdb (identified by file name pattern and header magic) are deleted; other files in the directory are never touched.

**Effect on point-in-time recovery**
- Point-in-time recovery can only reach times covered by the archive. The earliest recoverable time is exposed as the metric `sxdb_wal_archive_oldest_time_seconds` and in `/api/v1/status`.
- The server records the LSN of the most recent full backup. If cleanup deletes segments after that backup, the backup can no longer roll forward and can only be restored to its own point in time; a warning is logged when this happens. Full backups should therefore run more often than the retention period.

---

## 11. Security

### 11.1 Process Identity and Deployment
- The server **refuses to start** as root (uid 0), with no override. All default ports are above 1024, so root is never needed.
- A systemd unit is provided that:
  - runs as a dedicated `sxdb` user;
  - enables `NoNewPrivileges`, `ProtectSystem=strict`, `PrivateTmp`, `SystemCallFilter=@system-service`;
  - clears `CapabilityBoundingSet`;
  - allows writes only to the data and backup directories.
- On Linux, Landlock can optionally restrict file access to the data and backup directories.
- The process umask is 077. Startup is refused if the data directory or the configuration file has permissions that are too wide.
- WAL archiving and backups are done by built-in code; no shell commands are executed.

### 11.2 Network and Authentication
- Listens on 127.0.0.1 only by default; external addresses must be configured explicitly.
- Authentication is on by default and there is no default password; the administrator password is set by `sxdb init`.
- Authentication per protocol:
  - PG protocol: SCRAM-SHA-256; the server stores only salted, iterated verifiers.
  - HTTP and line protocol: tokens; the server stores only token hashes.
- Privileges: `SELECT`, `INSERT`, `DDL`, `ADMIN`, granted per database or table, with built-in read-only, read-write, and administrator roles.
- Protection against resource exhaustion:
  - failed authentications are rate limited;
  - connections, per-query memory, query time, line length, and batch size are all capped.
- TLS is an optional build feature via Mbed TLS. Without it, a proxy in front can terminate TLS.
- Passwords, tokens, and other secrets are never logged. An audit log (logins, DDL, privilege changes) can be enabled (see §12.3).

### 11.3 Code
- All code that parses network input checks bounds against explicit lengths. Functions such as `strcpy` and `sprintf` are banned through compile-time checks.
- Hardening flags: `-fstack-protector-strong`, `-D_FORTIFY_SOURCE=3` (2 on older compilers), PIE; on Linux also `-Wl,-z,relro,-z,now`. CI builds with `-Wall -Wextra -Werror`.
- `SECURITY.md` describes how to report vulnerabilities.

---

## 12. Observability

Principle: **metrics must not slow down writes or queries.** Metrics come from only two sources:
- counters updated on events that happen anyway, such as segment commits and memory allocations;
- values computed from existing state when the metrics are requested.

Nothing is added to the write or query hot paths, there is no background sampling thread, and no directories are scanned.

### 12.1 Metrics

| Metric | Source | Performance impact |
|---|---|---|
| Service state | In-process state variable | None |
| Uptime | Start time recorded at startup, computed on request | None |
| File storage size | Counters adjusted when segment and WAL files are created or deleted (these moments already commit to the MANIFEST), read directly on request | None |
| Memory usage | The memory manager already keeps these counters. Process RSS is read once from the OS per request | None |
| Number of databases | Read from the catalog | None |
| Free disk space | One `statvfs` call per request | None |
| Average rows per block, per table and partition | Block and row counts added when segments are committed | None |

**Service states**

| State | Meaning |
|---|---|
| `starting` | Loading configuration and catalog |
| `recovering` | Replaying the WAL, with progress percentage |
| `running` | Serving normally |
| `read_only` | The disk is full or a write hit an IO error: writes are refused with a clear error, queries continue. Returns to `running` automatically once space is available (disk space is checked every few seconds, only in this state) |
| `stopping` | Shutting down: no new connections, in-flight requests finish, then a flush |

**File storage size** is reported separately for:
- data files (segments) and the series index, per database and per table;
- the WAL;
- the WAL archive.

These are file sizes in bytes and may differ slightly from `du`, because file systems allocate in blocks and backup hard links share blocks with the data directory.

**Memory usage**:
- the total limit and total used;
- usage per consumer: buffer pool, MemTable, queries, series index, connection buffers, fixed reservation;
- process RSS.

### 12.2 Access

| Access | Content | Authentication |
|---|---|---|
| `GET /health` | State only: 200 when `running`, 503 otherwise, with the state in the body | Not required; exposes nothing else |
| `GET /api/v1/status` | All metrics as JSON | Required |
| `GET /metrics` | Prometheus text format | Required (Prometheus supports bearer tokens) |
| SQL | `sx_status` (one summary row), `sx_storage` (per database and table), `sx_memory` (per consumer), `sx_databases` | Required |
| sxql | `\status` | Logged-in session |
| Operations command | `sxdb status` | Required |
| C API | `sxdb_status()`, the same information in embedded mode | — |
| systemd | `Type=notify`: systemd is told the service is ready only after WAL recovery completes, and `systemctl status` shows the current state. The sd_notify protocol is implemented in-house (one message to a Unix socket), without libsystemd | — |

```sql
SELECT * FROM sx_status;
--  state   | version | start_time          | uptime   | databases | memory_used | memory_limit | storage_bytes | disk_free_bytes
--  running | 0.1.0   | 2026-10-16 08:00:00 | 05:12:33 | 3         | 6658998272  | 8589934592   | 442381631488  | 1429365116928
```

**Prometheus metrics** carry only a per-database label, so the label count stays bounded. Per-table details are available through `sx_storage` in SQL, to avoid an explosion of series in Prometheus.

```
sxdb_build_info{version="0.1.0"} 1
sxdb_state{state="running"} 1
sxdb_start_time_seconds 1760572800
sxdb_uptime_seconds 18753
sxdb_databases 3
sxdb_storage_bytes{database="iot",kind="data"} 401234567890
sxdb_storage_bytes{database="iot",kind="index"} 1234567890
sxdb_wal_bytes 268435456
sxdb_wal_archive_bytes 10737418240
sxdb_wal_archive_oldest_time_seconds 1759968000
sxdb_disk_free_bytes 1429365116928
sxdb_memory_limit_bytes 8589934592
sxdb_memory_used_bytes{pool="buffer_pool"} 5368709120
sxdb_memory_used_bytes{pool="memtable"} 1073741824
sxdb_process_resident_bytes 6710886400
```

### 12.3 Logging

```ini
log_level      = warn            # error | warn | info | debug; default warn
log_output     = file            # file | stderr
log_dir        = /var/log/sxdb
log_file_size  = 1G              # start a new file once the current one exceeds this size
log_total_size = 4G              # cap on all log files together; the oldest are deleted beyond it
```

**Format**: plain text, one event per line, English only:
```
2026-10-16T08:00:00.123Z WARN  [wal] fsync took 1532 ms, group commit delayed
```
- Each line holds: UTC time (millisecond precision), level, module, message.
- Newlines and control characters in messages are escaped, so one event is always one line and log injection is prevented.
- Passwords, tokens, and other secrets are never logged (see §11.2).

**Rotation and retention**
- The current file is `sxdb.log`. When it exceeds `log_file_size`, it is renamed to `sxdb-<UTC time>.log` (for example `sxdb-20261016T080000Z.log`) and a new `sxdb.log` is opened.
- When all log files together exceed `log_total_size`, the oldest are deleted. Only files matching this naming pattern are deleted.
- `log_total_size` must be at least `log_file_size`, otherwise startup is refused.
- Log files have permissions 0600.

**Destination**
- deb/rpm installations write to files by default.
- The Docker image writes to stderr by default for the container runtime to collect; no rotation in that case.

**Performance**
- Worker threads only push a formatted line onto a queue; a background thread writes the file, so logging never blocks writes or queries.
- When the queue is full, INFO and DEBUG messages are dropped and counted; WARN and ERROR are never dropped.
- The default level is WARN, so normal operation produces almost no log output.

**Runtime change**: `ALTER SYSTEM SET log_level = 'info';`

**Audit log** (optional, see §11.2): written to a separate `audit.log` with the same rotation and retention rules.

---

## 13. Reliability and Testing
- Every metadata update follows the same sequence: write a temp file → fsync → rename → fsync the directory.
- Every page, WAL record, and MANIFEST record carries a CRC32C, computed with SSE4.2 / ARMv8 hardware instructions.
- Testing:
  - **Unit tests**: a minimal in-house test framework.
  - **SQL regression tests**: sqllogictest format.
  - **Fuzzing**: libFuzzer on the SQL parser, every protocol parser, every decoder, and the file format readers.
  - **Crash injection**: repeated `kill -9` with consistency checks after each, plus fault injection at every fsync and rename point.
  - **Sanitizers**: ASan, UBSan, TSan in CI.
  - **Static analysis**: clang-tidy, CodeQL.
  - **Upgrade tests**: data directories, WALs, and backups produced by the previous version must upgrade, restore, and read correctly with the new version (from 1.0 on).

---

## 14. Upgrades and Compatibility

### 14.1 Versioning

Semantic versioning, `MAJOR.MINOR.PATCH`:

| Part | Allowed changes |
|---|---|
| PATCH | Bug fixes only; no disk format or behavior changes; free to upgrade or downgrade |
| MINOR | New features. May add disk format features, but new versions must read old data |
| MAJOR | Incompatible format or behavior changes, with an upgrade path |

**Before 1.0 (0.x), disk format compatibility is not guaranteed**: moving between MINOR versions may require export and re-import. The rules above apply from 1.0 on.

### 14.2 Disk Format: Versions and Feature Flags
- Each file type (segment, WAL, MANIFEST, CATALOG, index segment) has its own format version, independent of the software version.
- The `FORMAT` file in the data directory records enabled feature flags, in three classes (borrowed from ext4):

| Class | An older binary that encounters it |
|---|---|
| compat | Ignores it and works normally |
| ro_compat | Can open the data directory read-only only |
| incompat | Refuses to open and says "requires sxdb >= x.y" |

### 14.3 Upgrade Procedure
1. **Check**: `sxdb upgrade --check` checks the data directory format and whether the configuration file is valid for the new version, and lists deprecated settings.
2. **Pre-upgrade snapshot**: before rewriting any metadata, `sxdb upgrade` snapshots the whole data directory with hard links. Data files are immutable, so the snapshot is instant and takes almost no space. If anything goes wrong, roll back to the snapshot.
3. **Metadata**: on first start, the new version rewrites CATALOG and MANIFEST in the new format and switches atomically through `CURRENT`.
4. **Data files are read old, written new**: the new version reads old-format segments and index segments directly, writes new files in the new format, and old files are rewritten gradually by compaction, with no downtime for a full conversion. Cold partitions may never be compacted again; `OPTIMIZE TABLE ... REWRITE` or `sxdb upgrade --rewrite` rewrites them on demand.
5. **WAL**: the new version can replay the previous version's WAL, so upgrading works even after the old version crashed.
6. **Finalize**:
   - After the upgrade, features that need new format flags stay disabled, and switching back to the old version is still possible.
   - Once the administrator is satisfied, `sxdb upgrade --finalize` writes the new incompat flags and enables the new features.
   - The pending-finalize state is shown in `sx_status` and `/api/v1/status`.
7. **Supported range**: direct upgrades from the previous MAJOR version; across several MAJOR versions, upgrade one step at a time. Backups from older versions follow the same rule.
8. **Downtime**: a clean shutdown flushes the MemTable, so restart needs no WAL replay and upgrade downtime is a matter of seconds.

### 14.4 Interface Compatibility

| Interface | Policy |
|---|---|
| SQL behavior | Changes only in MAJOR versions. Syntax to be removed is deprecated with warnings at least one MINOR version earlier |
| Configuration file | Strict validation: unknown settings refuse startup, so a typo such as a misspelled `memory_limit` is never silently ignored. Deprecated settings warn and are removed in the next MAJOR version |
| HTTP API | Version in the path (`/api/v1/`). Incompatible changes go to `/api/v2/`; old versions stay for at least one MAJOR version |
| PG protocol, line protocol | Follow the external standards; stable |
| C API / ABI | The shared library has a SONAME version (`libsxdb.so.1`); the ABI is stable within a MAJOR version. The public API uses only opaque handles and setter functions and exposes no struct layouts, so new options never break the ABI. The only exception is the Arrow C Data Interface, whose structs are fixed by the Arrow specification |
| Python client | Versioned independently; talks to the server through `/api/v1/` |

---

## 15. Engineering and Release

### 15.1 Directory Layout

```
sxdb/
├── .github/                CI and release pipelines (GitHub Actions)
├── .clang-format           formatting configuration (see §15.2)
├── .clang-tidy             static analysis configuration
├── LICENSE                 GPL-2.0
├── README.md
├── include/sxdb.h          public embedded API
├── src/
│   ├── common/             arena, hash, crc32c, varint, roaring, epoch (EBR), log, config
│   ├── os/                 file IO (O_DIRECT / F_NOCACHE), event loop, threads, system info (cgroup)
│   ├── mem/                memory manager, buffer pool
│   ├── encoding/           bitpack, for, delta, alp, alp_rd, rle, dict, string
│   ├── storage/            wal, memtable, segment, partition, manifest, compaction,
│   │                       series_index, symbol_dict, catalog, bulk_import, last_cache,
│   │                       views (continuous aggregates), format (feature flags)
│   ├── sql/                lexer, parser, binder, planner, optimizer
│   ├── exec/               vector, scan, filter, project, agg, window, asof_join,
│   │                       hash_join, sort, gapfill, functions/
│   ├── arrow/              IPC read/write, minimal FlatBuffers encoder, C Data Interface
│   ├── server/             pgwire, ilp, http, session, auth
│   ├── crypto/             sha256, hmac, pbkdf2
│   └── backup/             backup, restore, wal_archive, verify
├── tools/
│   ├── sxdb-server/        standalone server
│   ├── sxdb/               operations: init, status, backup, restore, verify, dump, upgrade
│   ├── sxql/               interactive SQL client (PG wire client, line editing, table rendering)
│   └── sxdb-bench/         benchmarks
├── clients/python/         Python client (MIT, with its own LICENSE)
├── third_party/            lz4, zstd, linenoise, mbedtls (optional)
├── packaging/              systemd unit, Dockerfile, Homebrew formula (deb/rpm generated by CPack)
├── docs/
└── tests/                  unit, sqllogictest, fuzz, crash
```

The build system is CMake.

### 15.2 Coding Standards

**Readable, maintainable code**
- Write code with a clear structure and clear layering that people can read and maintain easily. Never write compressed, overly clever code that is hard for people to follow.
- **Layering**: modules depend only on layers below them, never on layers above, and there are no circular dependencies. From bottom to top: `os`, `common` → `mem` → `encoding` → `storage` → `sql`, `exec`, `arrow` → `backup`, `server` → `tools`.
- **Structure**: each file has one clear responsibility, described in a header comment. Each function does one thing and stays short enough to read in one sitting; split long functions into well-named helpers.
- **Naming**: descriptive names. Abbreviations only where they are standard in this domain (`lsn`, `ts`, `wal`, `sma`).
- **Expressions**: no nested ternaries, no multiple side effects in one expression, no assignments inside conditions. Prefer a few clear statements over one dense line.
- **Macros**: only where they clearly help (for example generating type-specialized kernels), and documented where they are defined.
- **Comments**: explain why, not what. Non-obvious invariants, ownership, and locking rules are documented next to the code they govern.
- **Performance-critical code** may be optimized aggressively, but it stays readable: hot loops and SIMD kernels live in clearly named functions with a comment explaining the technique, and each has a simple scalar reference implementation used in tests.
- **Error handling** follows one consistent pattern: check every return value and release resources through a single cleanup path (`goto cleanup` is acceptable for this).

**English only**
- All user-facing text is English only: error messages, logs, command-line help, system table contents, and state values.
- Code comments and repository documentation (README, `docs/`) are English only.
- **Enforced in CI**: source files and build scripts under `src/`, `include/`, `tools/`, and `tests/` fail CI if they contain non-ASCII characters.
  - Non-ASCII UTF-8 test data (for example CJK text) is written with escapes in C strings (such as `"\xe4\xb8\xad"`) or kept in data files under `tests/data/`, which is not checked.

**Error messages**
- Error codes use PostgreSQL SQLSTATE values, which drivers rely on to classify errors.
- Wording follows PostgreSQL's error message style: the primary message starts in lowercase without a trailing period; detail and hint are complete sentences.

**Formatting**
- **Pointers**: attach the star to the type: `char* p`, `const char* p`, `char** p`.
  - The same applies to pointer parameters and return types, for example `char* sx_strdup(const char* s);`.
  - Declarations must remain valid C. Declarators such as function pointers keep their normal syntax, for example `int (*cmp)(const void*, const void*)`; function pointer types used repeatedly get a typedef.
- **One variable per declaration**: write `char* a;` and `char* b;` on separate lines, never `char *a, *b;`.
- **Indentation and width**: 4 spaces, no tabs; a target line width of 100 columns.
- **Braces**: opening braces are attached to the statement; every control-flow body (`if`, `else`, `for`, `while`, `do`) has braces, even a single line.

**Tooling**
- `.clang-format` and `.clang-tidy` live at the repository root under version control and are added when implementation begins.
- `.clang-format` explicitly sets `PointerAlignment: Left` and `DerivePointerAlignment: false`, along with `IndentWidth: 4`, `UseTab: Never`, `ColumnLimit: 100`, and `BreakBeforeBraces: Attach`.
- Mandatory braces and one variable per declaration are checked with clang-tidy's `readability-braces-around-statements` and `readability-isolate-declaration`.
- CI fails if formatting or these checks fail.

```c
static char* sx_join(const char* a, const char* b, size_t* out_len) {
    size_t len_a = strlen(a);
    size_t len_b = strlen(b);
    char* buf = sx_alloc(len_a + len_b + 1);
    if (buf == NULL) {
        return NULL;
    }
    memcpy(buf, a, len_a);
    memcpy(buf + len_a, b, len_b + 1);
    *out_len = len_a + len_b;
    return buf;
}
```

**Naming prefixes**: public API symbols use the `sxdb_` prefix; internal symbols use `sx_`.

### 15.3 Hosting and CI
- **Hosting**: GitHub. Vulnerabilities are reported through GitHub private vulnerability reporting (see `SECURITY.md`). `CONTRIBUTING.md` describes contributions; a DCO check confirms every commit carries `Signed-off-by`.
- **CI (GitHub Actions)** on every pull request:
  - platforms: Linux x86_64 and arm64, macOS arm64 and x86_64;
  - compilers: GCC and Clang, plus an extra `-std=c23` build;
  - unit tests and sqllogictest;
  - ASan, UBSan, TSan;
  - clang-format and clang-tidy checks, non-ASCII check;
  - CodeQL static analysis.
- **Nightly jobs**: long fuzzing runs, crash injection, performance benchmarks, with alerts when performance regresses beyond a threshold. Benchmarks run on a dedicated machine, not on shared CI runners, which are too noisy for regression thresholds (see §16).
- **Pipeline security**: GitHub Actions pinned by commit SHA; minimal token permissions per job; a protected main branch where CI must pass before merging.

### 15.4 Release and Packaging

Pushing a `vX.Y.Z` tag builds and publishes:

| Artifact | Details |
|---|---|
| deb / rpm | Generated by CPack for x86_64 and arm64, targeting Debian 12+, Ubuntu 22.04+, RHEL/Rocky/Alma 8+. Split into `sxdb-server` (server, `sxdb` operations command, systemd unit), `sxql` (client), and `libsxdb` plus `libsxdb-dev`/`libsxdb-devel` (library and headers) |
| Homebrew | Initially in our own tap (`homebrew-sxdb`), with prebuilt arm64 and x86_64 bottles. `brew services` runs as the current user, not root |
| Docker image | amd64 and arm64, based on distroless (no shell), running as a non-root user. Published to GitHub Container Registry |

**deb/rpm installation**
- Creates the `sxdb` system user, `/var/lib/sxdb` (0700), and `/var/log/sxdb`.
- Installs `/etc/sxdb/sxdb.conf` owned by `root:sxdb` with permissions 0640: the service can read its configuration but cannot modify it.
- **The service is not started automatically after installation**: there is no default password, so `sxdb init` must set the administrator password first. The installer prints this step.

**Docker image**
- Data volume `/var/lib/sxdb`; logs go to stderr.
- The administrator password is passed through the file named by `SXDB_ADMIN_PASSWORD_FILE` (works with Docker secrets). Passing the password directly in an environment variable is not supported, because `docker inspect` shows environment variables.
- The health check uses `sxdb status --health`, so the image needs no curl.
- If the container's memory limit is lower than `memory_limit`, startup is refused (see §6.1).

**Signing and verification**
- Every release includes `SHA256SUMS`.
- deb/rpm packages are signed with GPG.
- Docker images and other artifacts are signed with cosign (keyless, via GitHub OIDC) and come with GitHub build provenance attestations.

**Distribution**: first deb/rpm packages on GitHub Releases; apt/yum repositories later.

---

## 16. Performance Targets and Validation

Benchmarks use the industry-standard TSBS (Time Series Benchmark Suite), comparing against InfluxDB, TDengine, and QuestDB on the same machine for the devops and iot scenarios, plus a financial tick data set. These are design targets to be validated by benchmarks:

| Metric | Target |
|---|---|
| Write throughput | ≥ 5 million points/s on an 8-core machine (batched writes) |
| Storage | ≤ 2 bytes/point (typical monitoring and market data) |
| Idle memory | < 20 MB (before the buffer pool has loaded data) |
| Aggregation over 100 million points | Sub-second; milliseconds when served by SMA |
| Decoding (lightweight encodings, data in CPU cache) | ≥ 1 value per CPU cycle per core |
| Encoding | ≥ 0.2 values per CPU cycle per core |

**Method**
- Micro-benchmarks report values per CPU cycle next to MB/s, plus hardware counters from `perf stat` (IPC, branch misses), so results compare across machines and point at the cause of a slowdown.
- Measurements run on a dedicated machine with a fixed CPU frequency policy, pinned to one NUMA node (`numactl --cpunodebind=0 --membind=0`). Every performance change is compared against a stored baseline from the same machine.
- Before the end-to-end comparison is possible, the same real data set is loaded into the compared databases to compare disk usage per row.

---

## 17. Roadmap

| Milestone | Scope | Deliverable |
|---|---|---|
| M0 | GitHub repository and CI; project skeleton, base libraries, logging, memory manager, encoders and micro-benchmarks; fused SIMD decode kernels with runtime dispatch | Compression ratios and codec speeds validated on real data; encoding formats finalized |
| M1 | Storage engine: WAL, MemTable, segments, MANIFEST, partitions, series index (with active bitmaps), buffer pool, crash recovery; `FORMAT` feature flags; storage and memory counters, `sxdb_status()` | Read/write through the embedded C API |
| M2 | SQL: parser, optimizer, vectorized executor, aggregates, time-series functions, window functions, ASOF JOIN; sxql local mode | SQL queries locally through sxql |
| M3 | Server: PG wire protocol, line protocol, HTTP, Arrow IPC read/write, Python client, authentication and privileges, secure deployment; sxql network mode; observability (`/health`, `/metrics`, status system tables, systemd notification) | sxql, psql, Grafana, Telegraf, pandas/polars, Prometheus can connect |
| M4 | Compaction, TTL, dead series cleanup, DELETE, bulk import, Last Cache, backup and restore (full, incremental, point-in-time) | Ready for trial use |
| M5 | Continuous aggregates; performance: SIMD kernels for executor operators, NUMA placement, parallel query tuning, SMA pushdown, spilling large hash tables and sorts to disk, TSBS comparison | Performance report |
| M6 | Hardening: continuous fuzzing, crash injection, security audit; `sxdb upgrade` (check, snapshot, finalize) and upgrade tests; PG binary COPY and `pg_catalog` (ADBC/ConnectorX); TLS; release pipeline (deb/rpm, Homebrew, Docker image, signing); documentation | v1.0 |

M0 starts with the encoders on purpose: compression ratio and decoding speed set the performance ceiling of the whole system, so they should be validated on real data as early as possible.

---

## 18. Open Items

None.

Note: the default port 9440 is also ClickHouse's default native-protocol TLS port. If both run on the same machine, one of them must change.

---

## Appendix A: Example Configuration File

```ini
# /etc/sxdb/sxdb.conf    permissions must be 0600 or 0640
# Unknown settings refuse startup (see §14.4).
# Settings changed at runtime with ALTER SYSTEM are stored in <data_dir>/sxdb.auto.conf
# and override the same settings in this file at startup (as in PostgreSQL).
# The server process cannot modify this file.

data_dir            = /var/lib/sxdb

# Memory (fixed values only)
memory_limit        = 8G
memtable_limit      = 2G
query_memory_limit  = 4G
io_mode             = direct        # direct | buffered

# WAL
wal_fsync           = 1s            # always | 1s | off
wal_archive_dir     = /backup/sxdb/wal
wal_archive_retention_time = 7d     # whichever of these two is reached first
wal_archive_retention_size = 10G    # deletes the oldest archives

# Threads
io_threads          = 2
write_shards        = 4
query_threads       = 16
background_threads  = 2
numa                = auto          # auto | off

# Network (localhost only by default)
pg_listen           = 127.0.0.1:5440
http_listen         = 127.0.0.1:8440
ilp_listen          = 127.0.0.1:9440
tls                 = off

# Logging
log_level           = warn          # error | warn | info | debug
log_output          = file          # file | stderr
log_dir             = /var/log/sxdb
log_file_size       = 1G
log_total_size      = 4G

# Limits
max_connections     = 1024
query_timeout       = 300s
max_line_length     = 1M
```
