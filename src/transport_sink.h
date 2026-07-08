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

#ifndef VSQL_STAT_SINKS_CH_TRANSPORT_SINK_H
#define VSQL_STAT_SINKS_CH_TRANSPORT_SINK_H

#include <cstring>
#include <string>
#include <vector>

#include "ch_native_sink.h"
#include "core/event_row.h"
#include "core/sink.h"
#include "http_sink.h"

namespace vsql_stat_ch {

// Dispatches each flush to one of two ClickHouse transports -- native protocol
// (port 9000, via clickhouse-c) or HTTP (port 8123, JSONEachRow via libcurl) --
// selected live by the `transport` sysvar. Runs only on the core's single flush
// worker, so reading the sysvar per flush is race-free with respect to sink
// state. A live `SET GLOBAL vsql_stat_ch.transport = 'http'` takes effect on
// the next flush.
class TransportSink : public ::vsql_stat::Sink {
public:
  // `transport` points at the sysvar-backed global ("native" or "http", read
  // per flush). The two sub-sinks own their own config pointers.
  TransportSink(char **transport,
                const vsql_stat_ch_native::ChNativeSink::Config &native_cfg,
                const vsql_stat_http::HttpSink::Config &http_cfg)
      : transport_(transport), native_(native_cfg), http_(http_cfg) {}

  bool flush(const std::vector<::vsql_stat::EventRow> &batch,
             std::string &err) override {
    if (is_http())
      return http_.flush(batch, err);
    return native_.flush(batch, err);
  }

private:
  bool is_http() const {
    const char *t = (transport_ && *transport_) ? *transport_ : "native";
    return std::strcmp(t, "http") == 0;
  }

  char **transport_;
  vsql_stat_ch_native::ChNativeSink native_;
  vsql_stat_http::HttpSink http_;
};

} // namespace vsql_stat_ch

#endif // VSQL_STAT_SINKS_CH_TRANSPORT_SINK_H
