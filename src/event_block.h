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

#ifndef VSQL_STAT_SINKS_CH_EVENT_BLOCK_H
#define VSQL_STAT_SINKS_CH_EVENT_BLOCK_H

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

// Declarations only (the implementation compiles in clickhouse_c_impl.c).
#include "clickhouse.h"

#include "core/event_row.h"

namespace vsql_stat_ch {

// A row batch transposed into one ClickHouse columnar block (ClickHouse is
// columnar on the wire). Shared by both transports: the native sink sends it
// as a Data packet, the HTTP sink serializes it as a `FORMAT Native` body.
//
// The block builder does NOT copy the slabs it is handed (they "must outlive
// chc_block_write"), so this object owns every backing buffer and column node
// until it is destroyed. Build once per flush; do not reuse.
class EventBlock {
public:
  explicit EventBlock(const chc_alloc *al) : al_(al) {}
  ~EventBlock();

  EventBlock(const EventBlock &) = delete;
  EventBlock &operator=(const EventBlock &) = delete;

  struct Options {
    // Round query_time_secs/lock_time_secs to 6 decimal places, reproducing
    // the values the former JSONEachRow HTTP body stored ("%.6f").
    bool round_secs_to_micros = false;
  };

  // Transpose `batch` (non-empty) into columns, in events_raw schema order.
  // Returns false with `err` set if a column type fails to parse.
  bool build(const std::vector<::vsql_stat::EventRow> &batch,
             const Options &opts, std::string &err);

  const chc_block_builder *builder() const { return &bb_; }

  // Comma-separated names of the built columns, in block order, for the
  // INSERT column list.
  std::string column_list() const;

private:
  static constexpr size_t kMaxColumns = 45;

  bool parse_type(const char *name, chc_type **out, std::string &err);

  const chc_alloc *al_;

  // Parsed column types, destroyed with this object.
  chc_type *t_lc_ = nullptr, *t_string_ = nullptr, *t_u64_ = nullptr,
           *t_u32_ = nullptr, *t_u16_ = nullptr, *t_bool_ = nullptr,
           *t_f64_ = nullptr, *t_dt64_ = nullptr;

  // Backing slabs and column nodes. deque: push_back never moves existing
  // elements, so pointers handed to the builder stay valid.
  std::deque<std::vector<uint8_t>> bytes_;
  std::deque<std::vector<uint64_t>> offsets_;
  std::deque<std::vector<uint32_t>> keys_;
  std::deque<chc_column> nodes_;

  chc_block_col cols_[kMaxColumns] = {};
  chc_block_builder bb_ = {};
};

} // namespace vsql_stat_ch

#endif // VSQL_STAT_SINKS_CH_EVENT_BLOCK_H
