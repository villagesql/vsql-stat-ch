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

#include "http_sink.h"

#include <curl/curl.h>

#include <cstdio>
#include <string>

// Declarations only (the implementation compiles in clickhouse_c_impl.c).
// CHC_PROVIDE_STDLIB_ALLOC exposes the chc_alloc_stdlib() declaration.
#define CHC_PROVIDE_STDLIB_ALLOC
#include "clickhouse.h"

#include "event_block.h"

namespace vsql_stat_http {

namespace {

using ::vsql_stat::EventRow;
using ::vsql_stat_ch::EventBlock;

size_t discard_cb(char *, size_t size, size_t nmemb, void *) {
  return size * nmemb; // we don't need the response body
}

// chc_io write callback that appends into a std::string (ud). Called from the
// C block writer, so it must not let an exception escape.
int append_to_string(void *ud, const void *buf, size_t len, chc_err *err) {
  try {
    static_cast<std::string *>(ud)->append(static_cast<const char *>(buf), len);
    return CHC_OK;
  } catch (...) {
    snprintf(err->msg, sizeof(err->msg), "out of memory building body");
    return CHC_ERR_OOM;
  }
}

// The batch as one ClickHouse Native-format block: the same columnar layout
// the native transport sends, minus the TCP-only BlockInfo prefix and
// per-column custom-serialization byte (FORMAT Native over HTTP carries
// neither). Sets `columns` to the INSERT column list matching the block.
bool build_body(const std::vector<EventRow> &batch, std::string &body,
                std::string &columns, std::string &err) {
  const chc_alloc al = chc_alloc_stdlib();
  EventBlock block(&al);
  // Float64 timings keep the 6-decimal values the former JSONEachRow body
  // stored ("%.6f").
  EventBlock::Options opts;
  opts.round_secs_to_micros = true;
  if (!block.build(batch, opts, err))
    return false;
  columns = block.column_list();

  chc_io io{};
  io.ud = &body;
  io.write = append_to_string;
  chc_block_opts wire{};
  wire.has_block_info = false;
  wire.has_custom_serialization = false;
  chc_err cerr;
  chc_err_reset(&cerr);
  if (chc_block_write(&io, block.builder(), &wire, &cerr) != CHC_OK) {
    err = std::string("encode Native block failed: ") + cerr.msg;
    return false;
  }
  return true;
}

std::string cfg(char **p) { return (p && *p) ? std::string(*p) : ""; }

} // namespace

bool HttpSink::flush(const std::vector<EventRow> &batch, std::string &err) {
  if (batch.empty())
    return true;

  const std::string url = cfg(config_.url);
  if (url.empty()) {
    err = "url not configured";
    return false;
  }
  const std::string database = cfg(config_.database);
  const std::string table = cfg(config_.table);
  const std::string user = cfg(config_.user);
  const std::string password = cfg(config_.password);
  const long http_timeout_secs = static_cast<long>(
      (config_.http_timeout_secs && *config_.http_timeout_secs > 0)
          ? *config_.http_timeout_secs
          : 10);

  CURL *curl = curl_easy_init();
  if (curl == nullptr) {
    err = "curl_easy_init failed";
    return false;
  }

  std::string body, columns;
  if (!build_body(batch, body, columns, err)) {
    curl_easy_cleanup(curl);
    return false;
  }

  // POST <url>/?query=INSERT INTO <db>.<table> (<columns>) FORMAT Native
  char *db_esc = curl_easy_escape(curl, database.c_str(), 0);
  char *tbl_esc = curl_easy_escape(curl, table.c_str(), 0);
  const std::string insert =
      std::string("INSERT INTO ") + (db_esc ? db_esc : "default") + "." +
      (tbl_esc ? tbl_esc : "events_raw") + " (" + columns + ") FORMAT Native";
  if (db_esc)
    curl_free(db_esc);
  if (tbl_esc)
    curl_free(tbl_esc);
  char *query_esc = curl_easy_escape(curl, insert.c_str(), 0);
  const std::string full_url = url + "/?query=" + (query_esc ? query_esc : "");
  if (query_esc)
    curl_free(query_esc);

  curl_easy_setopt(curl, CURLOPT_URL, full_url.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_cb);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, http_timeout_secs);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  if (!user.empty()) {
    curl_easy_setopt(curl, CURLOPT_USERNAME, user.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, password.c_str());
  }

  const CURLcode rc = curl_easy_perform(curl);
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK) {
    err = std::string("HTTP insert failed: ") + curl_easy_strerror(rc);
    return false;
  }
  if (http_code < 200 || http_code >= 300) {
    err = "endpoint returned HTTP " + std::to_string(http_code);
    return false;
  }
  return true;
}

} // namespace vsql_stat_http
