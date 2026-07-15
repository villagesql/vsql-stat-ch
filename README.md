# vsql-stat-ch — ClickHouse telemetry sink

A VillageSQL extension that captures one event per finished statement and ships
it to ClickHouse, over either of two transports selected by the `transport`
sysvar. Part of the `vsql-stat` family; shares the capture pipeline in
[`vsql-stat-core`](https://github.com/villagesql/vsql-stat-core) (a git submodule
under `core/`).

- **`transport = native`** (default, port 9000) — a columnar Data block via
  ClickHouse's own client (`clickhouse-c`). Higher fidelity: the row batch is
  transposed to columns, low-cardinality strings (`user`, `client_ip`, `schema`,
  `sql_command`) are dictionary-encoded, and each block is LZ4/ZSTD compressed.
  Because dictionaries and compression are per-block, native defaults to a larger
  `batch_max` (10000) — bigger blocks pack better.
- **`transport = http`** (port 8123) — a `INSERT … FORMAT JSONEachRow` POST via
  libcurl. Simpler and dependency-light on the wire; use it for low/moderate
  volume, or where the native port isn't reachable.

`transport` is live-switchable: `SET GLOBAL vsql_stat_ch.transport = 'http'`
takes effect on the next flush.

## Status

Preview. Requires a server started with `--vsql_allow_preview_extensions=ON`.

## Build

```bash
git clone --recurse-submodules <this-repo>   # pulls core + clickhouse-c submodules
export VillageSQL_BUILD_DIR=$HOME/build/villagesql
./build.sh               # build the VEB
./test.sh                # build + run the MTR suite
```

If cloned without `--recurse-submodules`:

```bash
git submodule update --init   # core (shared pipeline) + third_party/clickhouse-c
```

Prerequisites: a VillageSQL build dir (`VillageSQL_BUILD_DIR`), CMake 3.16+, a
C++17 compiler, and lz4 + zstd development libraries (`liblz4-dev libzstd-dev`
on Debian/Ubuntu, `brew install lz4 zstd` on macOS). The build produces
`build/vsql_stat_ch.veb`.

## Usage

Native transport (default):

```sql
INSTALL EXTENSION vsql_stat_ch;

SET GLOBAL vsql_stat_ch.transport         = 'native';   -- default
SET GLOBAL vsql_stat_ch.clickhouse_host   = 'clickhouse';
SET GLOBAL vsql_stat_ch.clickhouse_port   = 9000;
SET GLOBAL vsql_stat_ch.clickhouse_database = 'default';
SET GLOBAL vsql_stat_ch.clickhouse_table  = 'events_raw';
SET GLOBAL vsql_stat_ch.clickhouse_user   = 'default';
SET GLOBAL vsql_stat_ch.clickhouse_password = '...';
SET GLOBAL vsql_stat_ch.compression       = 1;   -- 0=none, 1=lz4, 2=zstd

SET GLOBAL vsql_stat_ch.enabled = ON;   -- start capturing
```

HTTP transport:

```sql
SET GLOBAL vsql_stat_ch.transport        = 'http';
SET GLOBAL vsql_stat_ch.clickhouse_url   = 'http://clickhouse:8123';
SET GLOBAL vsql_stat_ch.http_timeout_secs = 10;
-- clickhouse_database / _table / _user / _password shared with native
SET GLOBAL vsql_stat_ch.enabled = ON;
```

Sysvars:

| Variable | Transport | Purpose |
|----------|-----------|---------|
| `transport` | both | `native` (9000) or `http` (8123) |
| `clickhouse_database`, `clickhouse_table`, `clickhouse_user`, `clickhouse_password` | both | target + auth |
| `clickhouse_host`, `clickhouse_port`, `compression` | native | native socket + block codec (0=none,1=lz4,2=zstd) |
| `clickhouse_url`, `http_timeout_secs` | http | HTTP endpoint + timeout |

Shared behavior sysvars (`enabled`, `queue_capacity`, `batch_max`,
`flush_interval_ms`, `statement_max_bytes`) and status vars (`events_captured`,
`events_archived`, `events_dropped`, `queue_depth`, `flush_errors`,
`last_flush_utime`) come from the shared core.

## ClickHouse side (operator-owned)

The sink only *inserts*; you create the target table. The sink writes a
columnar block whose column **names, types, and order** must match the table
below — this is the canonical schema. Physical properties are yours to tune
(engine, `ORDER BY`, `PARTITION BY`, `TTL`, replication); the column
definitions are the contract.

```sql
CREATE TABLE default.events_raw
(
    event_time         DateTime64(6)          COMMENT 'Statement start time (microsecond precision).',
    user               LowCardinality(String) COMMENT 'Authenticated user (priv_user).',
    client_ip          LowCardinality(String) COMMENT 'Client IP; empty for socket/pipe connections.',
    schema             LowCardinality(String) COMMENT 'Default database at execution time.',
    sql_command        LowCardinality(String) COMMENT 'SQL command class, e.g. SELECT, INSERT.',
    connection_id      UInt64                 COMMENT 'Server connection/thread id.',
    in_transaction     Bool                   COMMENT 'Statement ran inside an open transaction.',
    query_time_secs    Float64                COMMENT 'Wall-clock execution time in seconds.',
    lock_time_secs     Float64                COMMENT 'Time spent waiting on locks, in seconds.',
    rows_sent          UInt64                 COMMENT 'Rows returned to the client.',
    rows_examined      UInt64                 COMMENT 'Rows read to satisfy the statement.',
    rows_affected      UInt64                 COMMENT 'Rows changed (DML), 0 otherwise.',
    warning_count      UInt32                 COMMENT 'Warnings raised by the statement.',
    status             UInt16                 COMMENT 'MySQL error code; 0 on success.',
    digest_text        String                 COMMENT 'Normalized statement digest (literals stripped).',
    query              String                 COMMENT 'Statement text (rewritten/redacted when applicable).',
    sqlstate           LowCardinality(String) COMMENT '5-char SQLSTATE; empty on success.',
    error_message      String DEFAULT ''      COMMENT 'Error text; empty on success.',
    port               UInt16                 COMMENT 'Client TCP port; 0 for socket connections.',
    bytes_sent         UInt64                 COMMENT 'Bytes sent to the client for this statement.',
    bytes_received     UInt64                 COMMENT 'Bytes received from the client for this statement.',
    select_full_join       UInt32             COMMENT 'Joins with no usable index.',
    select_full_range_join UInt32             COMMENT 'Joins using a range on a reference table.',
    select_range           UInt32             COMMENT 'Range scans on the first table.',
    select_range_check     UInt32             COMMENT 'Joins re-checking a key per row (no index).',
    select_scan            UInt32             COMMENT 'Full scans of the first table.',
    sort_merge_passes  UInt32                 COMMENT 'Sort merge passes (high = large sort).',
    sort_range         UInt32                 COMMENT 'Sorts done over a range.',
    sort_rows          UInt64                 COMMENT 'Rows sorted.',
    sort_scan          UInt32                 COMMENT 'Sorts done over a full scan.',
    created_tmp_tables      UInt32            COMMENT 'Internal temp tables created.',
    created_tmp_disk_tables UInt32            COMMENT 'Internal temp tables spilled to disk.',
    no_index_used      Bool                   COMMENT 'Statement ran without any index.',
    no_good_index_used Bool                   COMMENT 'No good index was available.'
)
ENGINE = MergeTree
PARTITION BY toDate(event_time)
ORDER BY (event_time)
TTL toDateTime(event_time) + INTERVAL 90 DAY DELETE,
    toDateTime(event_time) + INTERVAL 1 DAY RECOMPRESS CODEC(ZSTD(8));
```

`event_time` is the MySQL statement start time; the extension sends microseconds
since epoch, which `DateTime64(6)` stores directly. `LowCardinality(String)`
columns must be declared as such so the dictionary encoding round-trips; the
native sink sends them dictionary-encoded.

TLS on the native protocol (port 9440) is not wired yet.

## Testing

`./test.sh` builds and runs the MTR suite (4 tests, one hermetic + one
skip-gated live per transport). The hermetic tests (`ch_native`,
`http_transport`) need no ClickHouse — they point at a dead endpoint and assert
the capture→queue→worker path fires. The `*_live` tests are skip-gated: they run
only when a ClickHouse is reachable (`test.sh` auto-detects one on
`http://127.0.0.1:8123`), inserting for real and reading the row back. A local
instance:

```bash
docker run -d --name vsql-ch -p 8123:8123 -p 9000:9000 \
  -e CLICKHOUSE_SKIP_USER_SETUP=1 clickhouse/clickhouse-server
```

## Updating the shared core

`core/` is a git submodule pointing at `vsql-stat-core`. To move to a later
core commit:

```bash
git -C core fetch && git -C core checkout <ref>
git add core && git commit -m "Bump core"
```

## License

GPL v2 — see LICENSE.
