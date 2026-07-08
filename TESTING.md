# Testing vsql_stat_ch

## Run the test suite (recommended)

```bash
export VillageSQL_BUILD_DIR=/path/to/villagesql/build
./test.sh            # build + stage the VEB + run MTR
RECORD=1 ./test.sh   # also regenerate the .result files
```

`test.sh` builds the VEB, stages it into a temporary veb-dir, and runs the suite
against the dev server. The default test is hermetic; the live test is
skip-gated on a reachable ClickHouse (`test.sh` auto-detects one on
`http://127.0.0.1:8123`).

## Run MTR manually

From the server's `mysql-test/` directory (the VEB must already be staged in the
server's veb-dir):

```bash
cd /path/to/villagesql/build/mysql-test
perl mysql-test-run.pl --suite=/path/to/vsql-stat-ch/mysql-test
```

Regenerate expected results after an intended change:

```bash
perl mysql-test-run.pl --suite=/path/to/vsql-stat-ch/mysql-test --record
```

## What the tests cover

**ch_native.test** (hermetic — no ClickHouse needed):
- Installs `vsql_stat_ch`, points it at a dead host, enables capture, runs a
  marker query.
- Asserts `events_captured > 0` (the POSTEXECUTE hook captured the statement)
  and `flush_errors > 0` (the worker's bounded connect to the dead host failed) —
  proving hook → queue → worker → connect fired end to end. The bounded connect
  makes the failure fast, so the test cannot hang.

**ch_native_live.test** (skip-gated — runs only when `VSQL_STAT_CH_URL` points
at a reachable ClickHouse):
- Points the sink at the real ClickHouse (native protocol, port 9000), runs a
  unique marker query, waits for the worker to archive it (`events_archived > 0`,
  `flush_errors == 0`), then reads the row back over HTTP and asserts the payload.
  Exercises the row→column transpose and the LowCardinality columns end to end.

## Notes

- The default suite is hermetic and cannot hang.
- A local ClickHouse for the live test (exposes both the native 9000 and HTTP
  8123 ports; the test inserts over native and reads back over HTTP):
  ```bash
  docker run -d --name vsql-ch -p 8123:8123 -p 9000:9000 \
    -e CLICKHOUSE_SKIP_USER_SETUP=1 clickhouse/clickhouse-server
  ```
- `suite.opt` supplies `--vsql_allow_preview_extensions=ON`.
