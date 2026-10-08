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

#include "event_block.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace vsql_stat_ch {

namespace {

using ::vsql_stat::EventRow;

// Append `v`'s little-endian bytes to `out`. T must be a trivially-copyable
// arithmetic type; the platform is assumed little-endian (x86_64 / arm64), the
// only platforms this extension targets.
template <typename T> void put_le(std::vector<uint8_t> &out, T v) {
  const auto *p = reinterpret_cast<const uint8_t *>(&v);
  out.insert(out.end(), p, p + sizeof(T));
}

// `v` as text with 6 decimals and parsed back: exactly the double ClickHouse
// stored from the former JSONEachRow body, which printed it with "%.6f".
double round_to_micros(double v) {
  char buf[64];
  snprintf(buf, sizeof(buf), "%.6f", v);
  return strtod(buf, nullptr);
}

} // namespace

EventBlock::~EventBlock() {
  for (chc_type *t :
       {t_lc_, t_string_, t_u64_, t_u32_, t_u16_, t_bool_, t_f64_, t_dt64_})
    if (t)
      chc_type_destroy(t, al_);
}

bool EventBlock::parse_type(const char *name, chc_type **out,
                            std::string &err) {
  chc_err cerr;
  chc_err_reset(&cerr);
  if (chc_type_parse(name, strlen(name), al_, out, &cerr) != CHC_OK) {
    err = std::string("chc_type_parse(") + name + "): " + cerr.msg;
    return false;
  }
  return true;
}

std::string EventBlock::column_list() const {
  std::string out;
  for (size_t i = 0; i < bb_.n_cols; ++i) {
    if (i)
      out += ", ";
    out.append(bb_.cols[i].name, bb_.cols[i].name_len);
  }
  return out;
}

bool EventBlock::build(const std::vector<EventRow> &batch, const Options &opts,
                       std::string &err) {
  // These match the fixed events_raw schema (see schema/events_raw.sql):
  // event_time is DateTime64(6), the flag columns are Bool, the small
  // per-statement counters are UInt32, and sqlstate is LowCardinality(String).
  if (!parse_type("LowCardinality(String)", &t_lc_, err) ||
      !parse_type("String", &t_string_, err) ||
      !parse_type("UInt64", &t_u64_, err) ||
      !parse_type("UInt32", &t_u32_, err) ||
      !parse_type("UInt16", &t_u16_, err) ||
      !parse_type("Bool", &t_bool_, err) ||
      !parse_type("Float64", &t_f64_, err) ||
      !parse_type("DateTime64(6)", &t_dt64_, err))
    return false;

  const size_t n = batch.size();
  chc_block_builder_init(&bb_, cols_);

  auto append = [&](const char *name, const chc_type *t, chc_column col) {
    nodes_.push_back(col);
    chc_block_builder_append(&bb_, name, strlen(name), t, &nodes_.back());
  };

  // Fixed-width numeric column: a contiguous LE value buffer.
  auto fixed = [&](const char *name, const chc_type *t, auto get) {
    using T = decltype(get(batch.front()));
    bytes_.emplace_back();
    std::vector<uint8_t> &buf = bytes_.back();
    buf.reserve(n * sizeof(T));
    for (const EventRow &r : batch)
      put_le(buf, get(r));
    append(name, t, chc_build_fixed(buf.data(), sizeof(T), n));
  };

  // Plain String column: a data blob plus cumulative-end offsets.
  auto str = [&](const char *name, auto get) {
    bytes_.emplace_back();
    offsets_.emplace_back();
    std::vector<uint8_t> &data = bytes_.back();
    std::vector<uint64_t> &offs = offsets_.back();
    offs.reserve(n);
    for (const EventRow &r : batch) {
      const std::string &s = get(r);
      data.insert(data.end(), s.begin(), s.end());
      offs.push_back(data.size()); // cumulative exclusive end
    }
    append(name, t_string_, chc_build_string(offs.data(), data.data(), n));
  };

  // Non-Nullable LowCardinality(String) column: dedup values into a dictionary
  // and emit a UInt32 key per row. Unlike the Nullable variant, index 0 is a
  // real value (no empty-string sentinel) -- the first distinct value seen
  // takes key 0. (The Nullable convention of a slot-0 null sentinel does not
  // apply here; our columns are non-Nullable, so an empty field like client_ip
  // is just a normal dictionary entry.)
  auto lc = [&](const char *name, auto get) {
    keys_.emplace_back();
    bytes_.emplace_back();
    offsets_.emplace_back();
    std::vector<uint32_t> &keys = keys_.back();
    std::vector<uint8_t> &dict_data = bytes_.back();
    std::vector<uint64_t> &dict_offs = offsets_.back();
    keys.reserve(n);

    std::map<std::string, uint32_t> index;
    for (const EventRow &r : batch) {
      const std::string &s = get(r);
      auto it = index.find(s);
      uint32_t key;
      if (it == index.end()) {
        key = static_cast<uint32_t>(index.size());
        index.emplace(s, key);
        dict_data.insert(dict_data.end(), s.begin(), s.end());
        dict_offs.push_back(dict_data.size()); // cumulative exclusive end
      } else {
        key = it->second;
      }
      keys.push_back(key);
    }

    nodes_.push_back(
        chc_build_string(dict_offs.data(), dict_data.data(), dict_offs.size()));
    chc_column *dict = &nodes_.back();
    append(name, t_lc_, chc_build_lc(/*key_size=*/4, keys.data(), n, dict));
  };

  // Column order MUST match the INSERT column list and the events_raw schema.
  //
  // event_time is DateTime64(6): an Int64 count of microsecond ticks since
  // epoch. query_start_utime is already microseconds since epoch, so the value
  // is sent unchanged (DateTime64(6) tick == 1 microsecond).
  fixed("event_time", t_dt64_,
        [](const EventRow &r) { return r.query_start_utime; });
  lc("user", [](const EventRow &r) -> const std::string & { return r.user; });
  lc("client_ip",
     [](const EventRow &r) -> const std::string & { return r.client_ip; });
  lc("schema",
     [](const EventRow &r) -> const std::string & { return r.schema; });
  lc("sql_command",
     [](const EventRow &r) -> const std::string & { return r.sql_command; });
  fixed("connection_id", t_u64_,
        [](const EventRow &r) { return r.connection_id; });
  fixed("in_transaction", t_bool_,
        [](const EventRow &r) -> uint8_t { return r.in_transaction ? 1 : 0; });
  const bool round_secs = opts.round_secs_to_micros;
  fixed("query_time_secs", t_f64_, [round_secs](const EventRow &r) {
    return round_secs ? round_to_micros(r.query_time_secs) : r.query_time_secs;
  });
  fixed("lock_time_secs", t_f64_, [round_secs](const EventRow &r) {
    return round_secs ? round_to_micros(r.lock_time_secs) : r.lock_time_secs;
  });
  fixed("rows_sent", t_u64_, [](const EventRow &r) { return r.rows_sent; });
  fixed("rows_examined", t_u64_,
        [](const EventRow &r) { return r.rows_examined; });
  fixed("rows_affected", t_u64_,
        [](const EventRow &r) { return r.rows_affected; });
  // warning_count is UInt32 in the schema; narrow on emit.
  fixed("warning_count", t_u32_, [](const EventRow &r) -> uint32_t {
    return static_cast<uint32_t>(r.warning_count);
  });
  // status is UInt16 (the MySQL error code fits; 0 on success).
  fixed("status", t_u16_, [](const EventRow &r) -> uint16_t {
    return static_cast<uint16_t>(r.status);
  });
  str("digest_text",
      [](const EventRow &r) -> const std::string & { return r.digest_text; });
  str("digest_hash",
      [](const EventRow &r) -> const std::string & { return r.digest_hash; });
  str("query",
      [](const EventRow &r) -> const std::string & { return r.query; });
  // sqlstate is LowCardinality(String) in the schema: a 5-char SQLSTATE has few
  // distinct values, so it dictionary-encodes well.
  lc("sqlstate",
     [](const EventRow &r) -> const std::string & { return r.sqlstate; });
  str("error_message",
      [](const EventRow &r) -> const std::string & { return r.error_message; });
  fixed("port", t_u16_, [](const EventRow &r) -> uint16_t { return r.port; });
  fixed("bytes_sent", t_u64_, [](const EventRow &r) { return r.bytes_sent; });
  fixed("bytes_received", t_u64_,
        [](const EventRow &r) { return r.bytes_received; });
  // The optimizer/sort/tmp counters are UInt32 in the schema (they are small
  // per-statement deltas); narrow each on emit. sort_rows stays UInt64.
  fixed("select_full_join", t_u32_, [](const EventRow &r) -> uint32_t {
    return static_cast<uint32_t>(r.select_full_join);
  });
  fixed("select_full_range_join", t_u32_, [](const EventRow &r) -> uint32_t {
    return static_cast<uint32_t>(r.select_full_range_join);
  });
  fixed("select_range", t_u32_, [](const EventRow &r) -> uint32_t {
    return static_cast<uint32_t>(r.select_range);
  });
  fixed("select_range_check", t_u32_, [](const EventRow &r) -> uint32_t {
    return static_cast<uint32_t>(r.select_range_check);
  });
  fixed("select_scan", t_u32_, [](const EventRow &r) -> uint32_t {
    return static_cast<uint32_t>(r.select_scan);
  });
  fixed("sort_merge_passes", t_u32_, [](const EventRow &r) -> uint32_t {
    return static_cast<uint32_t>(r.sort_merge_passes);
  });
  fixed("sort_range", t_u32_, [](const EventRow &r) -> uint32_t {
    return static_cast<uint32_t>(r.sort_range);
  });
  fixed("sort_rows", t_u64_, [](const EventRow &r) { return r.sort_rows; });
  fixed("sort_scan", t_u32_, [](const EventRow &r) -> uint32_t {
    return static_cast<uint32_t>(r.sort_scan);
  });
  fixed("created_tmp_tables", t_u32_, [](const EventRow &r) -> uint32_t {
    return static_cast<uint32_t>(r.created_tmp_tables);
  });
  fixed("created_tmp_disk_tables", t_u32_, [](const EventRow &r) -> uint32_t {
    return static_cast<uint32_t>(r.created_tmp_disk_tables);
  });
  fixed("no_index_used", t_bool_,
        [](const EventRow &r) -> uint8_t { return r.no_index_used ? 1 : 0; });
  fixed("no_good_index_used", t_bool_, [](const EventRow &r) -> uint8_t {
    return r.no_good_index_used ? 1 : 0;
  });
  // Handler row-access counters (UInt64 -- row-read counts can be large).
  fixed("read_first", t_u64_, [](const EventRow &r) { return r.read_first; });
  fixed("read_last", t_u64_, [](const EventRow &r) { return r.read_last; });
  fixed("read_key", t_u64_, [](const EventRow &r) { return r.read_key; });
  fixed("read_next", t_u64_, [](const EventRow &r) { return r.read_next; });
  fixed("read_prev", t_u64_, [](const EventRow &r) { return r.read_prev; });
  fixed("read_rnd", t_u64_, [](const EventRow &r) { return r.read_rnd; });
  fixed("read_rnd_next", t_u64_,
        [](const EventRow &r) { return r.read_rnd_next; });
  // Client connection attributes: LowCardinality (few distinct pids/clients
  // per workload), like user/client_ip above.
  lc("client_pid",
     [](const EventRow &r) -> const std::string & { return r.client_pid; });
  lc("client_name",
     [](const EventRow &r) -> const std::string & { return r.client_name; });
  lc("program_name",
     [](const EventRow &r) -> const std::string & { return r.program_name; });
  return true;
}

} // namespace vsql_stat_ch
