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

#ifndef VSQL_STAT_SINKS_CH_NATIVE_CH_NATIVE_SINK_H
#define VSQL_STAT_SINKS_CH_NATIVE_CH_NATIVE_SINK_H

#include <string>
#include <vector>

#include "core/event_row.h"
#include "core/sink.h"

namespace vsql_stat_ch_native {

// ClickHouse native-protocol sink: streams a batch to ClickHouse over the
// native TCP protocol (port 9000) as a columnar Data block, using the vendored
// clickhouse-c client. Because ClickHouse is columnar on the wire, flush()
// transposes the row batch into per-column arrays before sending; low-
// cardinality string columns (user, sql_command, client_ip, schema) are sent
// dictionary-encoded. Reads its config through pointers to the sink's sysvar-
// backed globals, so a live SET GLOBAL is picked up on the next flush. Runs
// only on the core's single flush worker (no concurrent flush for this sink),
// so the persistent connection needs no locking.
class ChNativeSink : public ::vsql_stat::Sink {
public:
  struct Config {
    char **host;     // "localhost"
    char **database; // "default"
    char **table;    // "events_raw"
    char **user;     // "default"
    char **password;
    long long *port;        // 9000
    long long *compression; // 0=none, 1=lz4, 2=zstd
  };

  explicit ChNativeSink(const Config &config) : config_(config) {}
  ~ChNativeSink() override;

  ChNativeSink(const ChNativeSink &) = delete;
  ChNativeSink &operator=(const ChNativeSink &) = delete;

  bool flush(const std::vector<::vsql_stat::EventRow> &batch,
             std::string &err) override;

private:
  // Opaque connection state (clickhouse-c client + socket), owned here and held
  // across flushes. Defined in the .cc so the header never includes the
  // clickhouse-c headers. Null until the first successful connect.
  struct Conn;

  // Connect (or reconnect) to ClickHouse using the current config. On success
  // conn_ is non-null and true is returned; on failure err is set and false is
  // returned.
  bool ensure_connected(std::string &err);
  void disconnect();

  Config config_;
  Conn *conn_ = nullptr;
};

} // namespace vsql_stat_ch_native

#endif // VSQL_STAT_SINKS_CH_NATIVE_CH_NATIVE_SINK_H
