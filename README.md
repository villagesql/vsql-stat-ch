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

`./build.sh` keeps the submodules (`core`, `third_party/clickhouse-c`) synced to
the commit this checkout pins: if you cloned without `--recurse-submodules`, or a
later `git pull` advanced the pin, the script runs `git submodule update --init
--recursive` for you before configuring. (It leaves a submodule alone if it has
uncommitted local changes, so in-progress core work is never clobbered.)

**If you do NOT use `build.sh`** — calling `cmake` directly, building from an
IDE, or in CI — you must manage the submodules yourself; a plain `git clone` /
`git pull` does not populate or advance them:

```bash
git submodule update --init --recursive   # core (shared pipeline) + clickhouse-c
# optional: make `git pull` do this automatically from now on
git config submodule.recurse true
```

Prerequisites: a VillageSQL build dir (`VillageSQL_BUILD_DIR`), CMake 3.16+, a
C++17 compiler, and the lz4, zstd, and libcurl development libraries
(`liblz4-dev libzstd-dev libcurl4-openssl-dev` on Debian/Ubuntu;
`brew install lz4 zstd` on macOS, where libcurl ships with the system). The
build produces `build/vsql_stat_ch.veb`.

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

The sink only *inserts*; you create the target table. The canonical schema is
[`schema/events_raw.sql`](schema/events_raw.sql) — the
single source of truth (the live tests generate their tables from it, and it is
the shape the native sink's block builder writes). Apply it as-is, or copy it
and tune the physical properties:

```bash
clickhouse-client < schema/events_raw.sql
```

The column **names, types, and order** are a contract with the sink and must not
change. Everything after the closing paren (engine, `ORDER BY`, `PARTITION BY`,
`TTL`, replication) is a reasonable default you are free to change.

Notes:
- `event_time` is the MySQL statement start time; the extension sends
  microseconds since epoch, which `DateTime64(6)` stores directly.
- `LowCardinality(String)` columns must be declared as such so the dictionary
  encoding round-trips; the native sink sends them dictionary-encoded.

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
