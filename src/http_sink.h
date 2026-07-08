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

#ifndef VSQL_STAT_SINKS_HTTP_HTTP_SINK_H
#define VSQL_STAT_SINKS_HTTP_HTTP_SINK_H

#include <string>
#include <vector>

#include "core/event_row.h"
#include "core/sink.h"

namespace vsql_stat_http {

// HTTP sink: POSTs a batch to an HTTP endpoint. This first version reproduces
// exactly the prototype's behavior -- a ClickHouse JSONEachRow insert over the
// HTTP interface (POST <url>/?query=INSERT INTO <db>.<table> FORMAT
// JSONEachRow). Reads its config through pointers to the sink's sysvar-backed
// globals, so a live SET GLOBAL is picked up on the next flush. Runs only on
// the core's single flush worker.
class HttpSink : public ::vsql_stat::Sink {
public:
  struct Config {
    char **url;      // "http://host:8123" (empty disables sending)
    char **database; // "default"
    char **table;    // "events_raw"
    char **user;     // empty = no auth
    char **password;
    int64_t *http_timeout_secs;
  };

  explicit HttpSink(const Config &config) : config_(config) {}

  bool flush(const std::vector<::vsql_stat::EventRow> &batch,
             std::string &err) override;

private:
  Config config_;
};

} // namespace vsql_stat_http

#endif // VSQL_STAT_SINKS_HTTP_HTTP_SINK_H
