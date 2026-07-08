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

namespace vsql_stat_http {

namespace {

using ::vsql_stat::EventRow;

size_t discard_cb(char *, size_t size, size_t nmemb, void *) {
  return size * nmemb; // we don't need the response body
}

// Minimal JSON string escaping for the values we emit (control chars, quotes,
// backslashes). ClickHouse JSONEachRow expects standard JSON.
void append_json_string(std::string &out, const std::string &s) {
  out += '"';
  for (const char c : s) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        char buf[8];
        snprintf(buf, sizeof(buf), "\\u%04x", c);
        out += buf;
      } else {
        out += c;
      }
    }
  }
  out += '"';
}

void append_kv_str(std::string &out, const char *key, const std::string &val,
                   bool first) {
  if (!first)
    out += ',';
  out += '"';
  out += key;
  out += "\":";
  append_json_string(out, val);
}

void append_kv_num(std::string &out, const char *key, uint64_t val) {
  out += ",\"";
  out += key;
  out += "\":";
  out += std::to_string(val);
}

void append_kv_dbl(std::string &out, const char *key, double val) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%.6f", val);
  out += ",\"";
  out += key;
  out += "\":";
  out += buf;
}

// One JSONEachRow line per event.
std::string build_body(const std::vector<EventRow> &batch) {
  std::string body;
  body.reserve(batch.size() * 256);
  for (const EventRow &r : batch) {
    body += '{';
    append_kv_str(body, "query", r.query, /*first=*/true);
    append_kv_str(body, "user", r.user, false);
    append_kv_str(body, "client_ip", r.client_ip, false);
    append_kv_str(body, "schema", r.schema, false);
    append_kv_str(body, "sql_command", r.sql_command, false);
    append_kv_num(body, "connection_id", r.connection_id);
    append_kv_num(body, "in_transaction", r.in_transaction ? 1 : 0);
    append_kv_num(body, "query_start_utime", r.query_start_utime);
    append_kv_dbl(body, "query_time_secs", r.query_time_secs);
    append_kv_dbl(body, "lock_time_secs", r.lock_time_secs);
    append_kv_num(body, "rows_sent", r.rows_sent);
    append_kv_num(body, "rows_examined", r.rows_examined);
    append_kv_num(body, "rows_affected", r.rows_affected);
    append_kv_num(body, "warning_count", r.warning_count);
    append_kv_num(body, "status", static_cast<uint64_t>(r.status));
    append_kv_num(body, "port", r.port);
    append_kv_str(body, "sqlstate", r.sqlstate, false);
    append_kv_str(body, "error_message", r.error_message, false);
    append_kv_str(body, "digest_text", r.digest_text, false);
    append_kv_num(body, "bytes_sent", r.bytes_sent);
    append_kv_num(body, "bytes_received", r.bytes_received);
    append_kv_num(body, "select_full_join", r.select_full_join);
    append_kv_num(body, "select_full_range_join", r.select_full_range_join);
    append_kv_num(body, "select_range", r.select_range);
    append_kv_num(body, "select_range_check", r.select_range_check);
    append_kv_num(body, "select_scan", r.select_scan);
    append_kv_num(body, "sort_merge_passes", r.sort_merge_passes);
    append_kv_num(body, "sort_range", r.sort_range);
    append_kv_num(body, "sort_rows", r.sort_rows);
    append_kv_num(body, "sort_scan", r.sort_scan);
    append_kv_num(body, "created_tmp_tables", r.created_tmp_tables);
    append_kv_num(body, "created_tmp_disk_tables", r.created_tmp_disk_tables);
    append_kv_num(body, "no_index_used", r.no_index_used ? 1 : 0);
    append_kv_num(body, "no_good_index_used", r.no_good_index_used ? 1 : 0);
    body += "}\n";
  }
  return body;
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

  // POST <url>/?query=INSERT INTO <db>.<table> FORMAT JSONEachRow
  char *db_esc = curl_easy_escape(curl, database.c_str(), 0);
  char *tbl_esc = curl_easy_escape(curl, table.c_str(), 0);
  const std::string insert =
      std::string("INSERT INTO ") + (db_esc ? db_esc : "default") + "." +
      (tbl_esc ? tbl_esc : "events_raw") + " FORMAT JSONEachRow";
  if (db_esc)
    curl_free(db_esc);
  if (tbl_esc)
    curl_free(tbl_esc);
  char *query_esc = curl_easy_escape(curl, insert.c_str(), 0);
  const std::string full_url = url + "/?query=" + (query_esc ? query_esc : "");
  if (query_esc)
    curl_free(query_esc);

  const std::string body = build_body(batch);

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
