# AGENTS.md

This file provides guidance to AI coding assistants (Claude Code, Gemini Code Assist, etc.) when working with code in this repository.

**Note**: Also check `AGENTS.local.md` for additional local development instructions when present.

## Project Overview

This is a per-query telemetry extension for VillageSQL (a MySQL-compatible database). It captures one event per finished statement (query text, user, timing, rows, error state, ...) and ships it to ClickHouse over one of two transports, selected by the `transport` sysvar: **native** (port 9000, a columnar Data block via the vendored `clickhouse-c` client) or **http** (port 8123, an `INSERT … FORMAT JSONEachRow` POST via libcurl). Both ship in this one extension behind a `TransportSink` that dispatches per flush; `transport` is live-switchable. Part of the `vsql-stat` family; the shared capture pipeline lives in [`vsql-stat-core`](https://github.com/villagesql/vsql-stat-core), a git submodule at `core/`.

Unlike function extensions (VDFs), this uses VillageSQL **preview capabilities**: a POSTEXECUTE `statement_event` hook (capture on the connection thread) plus a `thread_worker` (background flush), with `sys_var`/`status_var` for config and observability.

## Build System

**IMPORTANT**: Always build in the `build/` directory, never in the source root.

**First-time setup**: clone with submodules and install deps before building —
see [Cloning](#cloning) and [Dependencies](#dependencies) below. (`build.sh`
auto-syncs submodules to the pin; CMake names any missing dep.)

`VillageSQL_BUILD_DIR` tells `build.sh`/`test.sh` where to find the VillageSQL
SDK + server. It may point at **either**:

- a **from-source build tree** (e.g. `$HOME/build/villagesql`), or
- a **prebuilt install** at `$HOME/.villagesql/prebuilt`, produced by the
  official installer without building the server from source:
  `INSTALL_METHOD=prebuilt bash -c "$(curl -fsSL https://install.villagesql.com)"`

Both carry `villagesql-extension-sdk-*/`, `mysql-test/`, and `bin/mysqld` at the
same relative paths, so build + MTR work against either. The prebuilt install is
the quickest way to develop/test an extension without a full server build.

**When unset, `build.sh`/`test.sh` default to `$HOME/.villagesql/prebuilt` if it
exists** — so after a vanilla `install.villagesql.com` (prebuilt) install, no env
var is needed at all; just run `./test.sh`. Set the var explicitly to use a
build tree or a non-default location.

### Build

```bash
export VillageSQL_BUILD_DIR=/path/to/villagesql/build   # or $HOME/.villagesql/prebuilt
./build.sh
```

`build.sh` selects the newest staged SDK under `VillageSQL_BUILD_DIR` and builds
`build/vsql_stat_ch.veb`.

### Test

```bash
export VillageSQL_BUILD_DIR=/path/to/villagesql/build   # or $HOME/.villagesql/prebuilt
./test.sh            # build + run the MTR suite
RECORD=1 ./test.sh   # build + record the .result files
```

`test.sh` builds, stages the VEB, and runs MTR. The `ch_native` test is hermetic
(dead-host, asserts the pipeline fires); `ch_native_live` is skip-gated and runs
only when a ClickHouse is reachable (`test.sh` auto-detects one on
`http://127.0.0.1:8123`). See `TESTING.md`.

### Cloning

This repo has **two git submodules**: the shared pipeline `vsql-stat-core` at
`core/`, and ClickHouse's `clickhouse-c` client at `third_party/clickhouse-c`.

```bash
git clone --recurse-submodules <this-repo>
# or, after a plain clone:
git submodule update --init
```

### Dependencies

- VillageSQL Extension Framework SDK — **preview** headers (`include-dev/`), for
  the statement_event / thread_worker / sys_var / status_var capabilities. The
  bundled `cmake/FindVillageSQL.cmake` selects `include-dev/` when
  `VillageSQL_USE_DEV_HEADERS` is ON (set in `CMakeLists.txt`).
- `clickhouse-c` — the native-protocol client, vendored as a submodule
  (header-only; compiled via one TU, `src/clickhouse_c_impl.c`, that defines
  `CHC_IMPLEMENTATION`).
- lz4 + zstd (`liblz4-dev libzstd-dev`, or `brew install lz4 zstd`) — the native
  protocol's block-compression codecs.
- libcurl (`libcurl4-openssl-dev` on Debian/Ubuntu; ships with macOS) — the HTTP
  transport (JSONEachRow POST). CMake hard-requires it (`find_package(CURL)`).

## Architecture

**Shared core (`core/`, vendored from vsql-stat-core):**
- `event_row.h` — the captured statement row.
- `event_queue.{h,cc}` — bounded MPSC queue (drop-newest overflow).
- `sink.h` — the `Sink` interface this extension implements.
- `capture_pipeline.{h,cc}` — the POSTEXECUTE hook (capture→enqueue) and the
  background worker (drain→`sink.flush`). All capture/buffering logic lives here.

**This sink (`src/`):**
- `extension.cc` — the SDK capability objects (statement_event hook,
  thread_worker, sysvars, statusvars), the `TransportSink` instance, static-init
  binding into the core pipeline, and `VEF_GENERATE_ENTRY_POINTS`.
- `transport_sink.{h}` — a `Sink` that holds both sub-sinks and dispatches each
  flush to the one named by the `transport` sysvar (`native` | `http`).
- `ch_native_sink.{h,cc}` — the native `Sink`: TCP connect (bounded timeout),
  native-protocol handshake/insert via clickhouse-c, and the **row→column
  transpose** (ClickHouse is columnar on the wire), with LowCardinality
  dictionary encoding for `user`/`client_ip`/`schema`/`sql_command`.
- `http_sink.{h,cc}` — the http `Sink`: builds a ClickHouse JSONEachRow body
  from the batch and POSTs it via libcurl.
- `clickhouse_c_impl.c` — the single TU that compiles the clickhouse-c
  implementation (`CHC_IMPLEMENTATION`).

**Config (sysvars, `vsql_stat_ch.*`):** `transport` (native|http),
`clickhouse_database`/`_table`/`_user`/`_password` (both transports);
`clickhouse_host`/`clickhouse_port`/`compression` (native); `clickhouse_url`/
`http_timeout_secs` (http); plus shared behavior sysvars (`enabled`,
`queue_capacity`, `batch_max`, `flush_interval_ms`, `statement_max_bytes`).
Integer sysvars use `int64_t` backing globals to match the SDK's fixed-width
ABI. Status vars: `events_captured`, `events_archived`, `events_dropped`,
`queue_depth`, `flush_errors`, `last_flush_utime`.

**All network/disk I/O runs on the worker, off the query path** — the hook only
filters + enqueues, so telemetry never adds query latency.

## Updating the shared core

`core/` is a git submodule pointing at `vsql-stat-core`. To move to a later core
commit:

```bash
git -C core fetch && git -C core checkout <ref>
git add core && git commit -m "Bump core"
```

Do NOT edit `core/` files directly here — change them in `vsql-stat-core`, then
bump the submodule. Core-internal includes are sibling-relative
(`#include "event_queue.h"`); this sink includes them as `#include "core/..."`.

## Licensing and Copyright

All source files (`.cc`, `.h`, `.c`) and `CMakeLists.txt` must carry this header:

```
// Copyright (c) 2026 VillageSQL Contributors
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2.0,
// as published by the Free Software Foundation.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License, version 2.0, for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
```

## Common Tasks for AI Agents

- **Adding a config knob**: add an `int64_t`/`char*` global + a `sv::make_int`/
  `make_str` entry in `src/extension.cc`, add it to the relevant sub-sink's
  `Config` (`ChNativeSink::Config` or `HttpSink::Config`), thread it through the
  `TransportSink` constructor, and consume it in that sink's `.cc`. Integer
  sysvars must use `int64_t` (the SDK's `make_int` takes `int64_t*`).
- **Changing the transport dispatch**: `transport_sink.h`.
- **Changing capture fields**: `EventRow` and the capture logic live in `core/`
  — change them in `vsql-stat-core` and bump the submodule, not here.
- **Changing the wire format / transpose**: `ch_native_sink.cc`. Remember the
  block builder does not copy slabs — per-column backing buffers must outlive
  the block write.
- **Testing**: add/update `mysql-test/t/*.test`, regenerate with
  `RECORD=1 ./test.sh`. Keep the default (`ch_native`) test hermetic; put
  real-ClickHouse assertions in the skip-gated live test.

Always maintain existing code style and include the copyright header.
