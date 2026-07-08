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

The sink only *inserts*. Create the target table yourself, declaring the
low-cardinality columns as `LowCardinality(String)` so the dictionary encoding
is preserved end to end:

```sql
CREATE TABLE default.events_raw
(
    query              String,
    user               LowCardinality(String),
    client_ip          LowCardinality(String),
    schema             LowCardinality(String),
    sql_command        LowCardinality(String),
    connection_id      UInt64,
    in_transaction     UInt8,
    query_start_utime  UInt64,
    query_time_secs    Float64,
    lock_time_secs     Float64,
    rows_sent          UInt64,
    rows_examined      UInt64,
    rows_affected      UInt64,
    warning_count      UInt64,
    status             Int32,
    digest_text        String,
    sqlstate           String,
    error_message      String,
    port               UInt16,
    bytes_sent         UInt64,
    bytes_received     UInt64,
    select_full_join       UInt64,
    select_full_range_join UInt64,
    select_range           UInt64,
    select_range_check     UInt64,
    select_scan            UInt64,
    sort_merge_passes  UInt64,
    sort_range         UInt64,
    sort_rows          UInt64,
    sort_scan          UInt64,
    created_tmp_tables      UInt64,
    created_tmp_disk_tables UInt64,
    no_index_used      UInt8,
    no_good_index_used UInt8
)
ENGINE = MergeTree
ORDER BY (query_start_utime);
```

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
