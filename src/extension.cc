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
// along with this program; if not, see <https://www.gnu.org/licenses/>.

// vsql_stat_ch: per-query telemetry streamed to ClickHouse, over either the
// native protocol (port 9000) or the HTTP interface (port 8123), selected by
// the `transport` sysvar.
//
// This extension wires the shared backend-agnostic capture pipeline (core/) to
// a ClickHouse sink. It ships both transports behind a TransportSink that
// dispatches per flush on `transport`: native (columnar block via the vendored
// clickhouse-c client) or http (JSONEachRow POST via libcurl). The
// capture/queue/flush logic lives in core/; this file owns only the SDK
// capability objects, the sysvars, and the entry-point composition.

#include <villagesql/preview/statement_event.h>
#include <villagesql/preview/status_var.h>
#include <villagesql/preview/sys_var.h>
#include <villagesql/preview/thread_worker.h>
#include <villagesql/vsql.h>

#include "core/capture_pipeline.h"
#include "transport_sink.h"

using namespace vsql;
namespace se = vsql::preview_statement_event;
namespace sv = vsql::preview_sys_var;
namespace stv = vsql::preview_status_var;
namespace tw = vsql::preview_thread_worker;

namespace {

// Sysvars. `transport` selects the wire: "native" (port 9000, columnar via
// clickhouse-c) or "http" (port 8123, JSONEachRow via libcurl). Native uses
// host/port/compression; http uses url/http_timeout_secs; both share
// database/table/user/password. The `enabled` switch is owned by the
// thread_worker capability below.
char *g_transport = nullptr; // "native" | "http"
char *g_database = nullptr;
char *g_table = nullptr;
char *g_user = nullptr;
char *g_password = nullptr;
// native transport
char *g_host = nullptr;
// Integer sys/status vars are declared as `long long` to match the status-var
// and sys-var SDK contract (make_int takes `long long *`). int64_t is `long` on
// LP64 Linux and `long long` on macOS, so using int64_t here compiled on macOS
// but failed to bind on Linux.
long long g_port = 9000;
long long g_compression = 1; // 0=none, 1=lz4, 2=zstd
// http transport
char *g_url = nullptr;
long long g_http_timeout_secs = 10;
// shared behavior
long long g_queue_capacity = 100000;
long long g_batch_max = 10000;
long long g_flush_interval_ms = 1000;
long long g_statement_max_bytes = 4096;

auto SYS_VARS = sv::make_capability({
    sv::make_str("transport",
                 "ClickHouse transport: 'native' (port 9000) or 'http' "
                 "(port 8123)",
                 &g_transport, "native"),
    sv::make_str("clickhouse_database", "Target ClickHouse database",
                 &g_database, "default"),
    sv::make_str("clickhouse_table", "Target ClickHouse table", &g_table,
                 "events_raw"),
    sv::make_str("clickhouse_user", "Auth user", &g_user, "default"),
    sv::make_str("clickhouse_password", "Auth password", &g_password, ""),
    // native transport
    sv::make_str("clickhouse_host",
                 "Native-protocol host (transport=native; empty disables "
                 "sending)",
                 &g_host, "localhost"),
    sv::make_int("clickhouse_port", "Native protocol port (transport=native)",
                 &g_port, 9000, 1, 65535),
    sv::make_int("compression",
                 "Native block codec (transport=native): 0=none, 1=lz4, 2=zstd",
                 &g_compression, 1, 0, 2),
    // http transport
    sv::make_str("clickhouse_url",
                 "HTTP endpoint, e.g. http://ch-host:8123 (transport=http; "
                 "empty disables sending)",
                 &g_url, ""),
    sv::make_int("http_timeout_secs",
                 "Timeout for an HTTP insert (transport=http)",
                 &g_http_timeout_secs, 10, 1, 300),
    // shared behavior
    sv::make_int("queue_capacity",
                 "Max buffered events before drop-newest overflow",
                 &g_queue_capacity, 100000, 1000, 100000000),
    sv::make_int("batch_max", "Flush when this many events are queued",
                 &g_batch_max, 10000, 1, 1000000),
    sv::make_int("flush_interval_ms", "Flush at least this often (ms)",
                 &g_flush_interval_ms, 1000, 10, 3600000),
    sv::make_int("statement_max_bytes",
                 "Truncate captured query text to this many bytes",
                 &g_statement_max_bytes, 4096, 0, 1048576),
});

// Status-var counters: long long to match make_int's `long long *` (see the
// note on the sys-var globals above).
long long g_events_captured = 0;
long long g_events_archived = 0;
long long g_events_dropped = 0;
long long g_queue_depth = 0;
long long g_flush_errors = 0;
long long g_last_flush_utime = 0;

auto STATUS_VARS = stv::make_capability({
    stv::make_int("events_captured", &g_events_captured),
    stv::make_int("events_archived", &g_events_archived),
    stv::make_int("events_dropped", &g_events_dropped),
    stv::make_int("queue_depth", &g_queue_depth),
    stv::make_int("flush_errors", &g_flush_errors),
    stv::make_int("last_flush_utime", &g_last_flush_utime),
});

// The sink: a TransportSink dispatching per flush on `transport`. It holds both
// sub-sinks, each reading its own config pointers; only the selected
// transport's config matters at a given flush.
vsql_stat_ch::TransportSink g_sink{&g_transport,
                                   {&g_host, &g_database, &g_table, &g_user,
                                    &g_password, &g_port, &g_compression},
                                   {&g_url, &g_database, &g_table, &g_user,
                                    &g_password, &g_http_timeout_secs}};

// Bind the sink + config/status pointers into the core pipeline once. Static-
// init order is irrelevant: init() captures the ADDRESSES of the globals above
// (fixed at link time), never their dynamically-initialized values.
const bool g_inited = [] {
  ::vsql_stat::init(&g_sink,
                    ::vsql_stat::Config{&g_queue_capacity, &g_batch_max,
                                        &g_flush_interval_ms,
                                        &g_statement_max_bytes},
                    ::vsql_stat::Status{&g_events_captured, &g_events_archived,
                                        &g_events_dropped, &g_queue_depth,
                                        &g_flush_errors, &g_last_flush_utime});
  return true;
}();

// Capability handlers (thin wrappers over the core pipeline).
void on_query(const se::StatementEventArgs &args,
              se::StatementEventResult & /*result*/) {
  ::vsql_stat::on_statement(args);
}

vef_next_wakeup_t flush_worker(vef_wakeup_reason_t reason,
                               struct vef_thread_handle_t * /*thread*/,
                               void * /*user_data*/) {
  return ::vsql_stat::on_flush(reason);
}

tw::ThreadWorkerCapability<&flush_worker> THREAD_WORKER{"flush", "enabled"};

se::StatementEventCapability<VEF_STATEMENT_EVENT_POSTEXECUTE, &on_query>
    STATEMENT_EVENT;

} // namespace

VEF_GENERATE_ENTRY_POINTS(make_extension()
                              .with(SYS_VARS)
                              .with(STATUS_VARS)
                              .with(STATEMENT_EVENT)
                              .with(THREAD_WORKER))
