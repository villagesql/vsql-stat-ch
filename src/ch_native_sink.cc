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

#include "ch_native_sink.h"

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Declarations only (the implementation compiles in clickhouse_c_impl.c, the
// single TU that defines CHC_IMPLEMENTATION). CHC_PROVIDE_STDLIB_ALLOC exposes
// the chc_alloc_stdlib() declaration so we can call it here.
#define CHC_PROVIDE_STDLIB_ALLOC
#include "clickhouse.h"

#include "clickhouse-client.h"
#include "clickhouse-compression.h"
#include "clickhouse-posix-io.h"

namespace vsql_stat_ch_native {

namespace {

using ::vsql_stat::EventRow;

std::string cfg(char **p) { return (p && *p) ? std::string(*p) : ""; }

// Connect one non-blocking socket to `ai` with a bounded timeout. Returns the
// connected fd (left blocking), or -1. A bounded connect matters: without it a
// dead/unroutable host would block the single flush worker for the OS default
// (tens of seconds), which is exactly the hang the hermetic dead-endpoint test
// must avoid.
int connect_with_timeout(struct addrinfo *ai, int timeout_ms) {
  int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
  if (fd < 0)
    return -1;

  const int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);

  int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
  if (rc != 0 && errno == EINPROGRESS) {
    struct pollfd pfd = {fd, POLLOUT, 0};
    rc = poll(&pfd, 1, timeout_ms);
    if (rc == 1) {
      int soerr = 0;
      socklen_t len = sizeof(soerr);
      getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len);
      rc = (soerr == 0) ? 0 : -1;
    } else {
      rc = -1; // timeout or poll error
    }
  }

  if (rc != 0) {
    close(fd);
    return -1;
  }
  fcntl(fd, F_SETFL, flags); // restore blocking
  return fd;
}

// Open a TCP connection to host:port with a bounded connect timeout. Returns
// the fd, or -1 on failure with `err` set.
int tcp_connect(const std::string &host, int port, std::string &err) {
  char port_str[16];
  snprintf(port_str, sizeof(port_str), "%d", port);

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo *res = nullptr;
  const int gai = getaddrinfo(host.c_str(), port_str, &hints, &res);
  if (gai != 0) {
    err = "getaddrinfo(" + host + "): " + gai_strerror(gai);
    return -1;
  }

  int fd = -1;
  for (struct addrinfo *ai = res; ai != nullptr; ai = ai->ai_next) {
    fd = connect_with_timeout(ai, /*timeout_ms=*/5000);
    if (fd >= 0)
      break;
  }
  freeaddrinfo(res);

  if (fd < 0)
    err = "connect(" + host + ":" + port_str + ") failed: " + strerror(errno);
  return fd;
}

} // namespace

// Connection state held across flushes. One flush worker, no concurrency, so
// no locking. Owns the socket fd, the clickhouse-c client, its allocator, the
// posix-io backend, and the selected codec.
struct ChNativeSink::Conn {
  int fd = -1;
  chc_alloc al{};
  chc_io io{};
  chc_posix_io posix{};
  chc_codec codec{};
  chc_client *client = nullptr;

  ~Conn() {
    if (client != nullptr)
      chc_client_close(client);
    if (fd >= 0)
      close(fd);
  }
};

ChNativeSink::~ChNativeSink() { disconnect(); }

void ChNativeSink::disconnect() {
  delete conn_;
  conn_ = nullptr;
}

bool ChNativeSink::ensure_connected(std::string &err) {
  if (conn_ != nullptr)
    return true;

  const std::string host =
      cfg(config_.host).empty() ? "localhost" : cfg(config_.host);
  const int port = (config_.port && *config_.port > 0)
                       ? static_cast<int>(*config_.port)
                       : 9000;

  auto conn = std::make_unique<Conn>();
  conn->fd = tcp_connect(host, port, err);
  if (conn->fd < 0)
    return false;

  conn->al = chc_alloc_stdlib();
  chc_posix_io_init(&conn->posix, &conn->io, conn->fd, /*check_cancel=*/nullptr,
                    /*cancel_ud=*/nullptr);

  const int64_t comp = config_.compression ? *config_.compression : 1;
  chc_client_opts opts;
  memset(&opts, 0, sizeof(opts));
  opts.database = *config_.database ? *config_.database : "default";
  opts.user = *config_.user ? *config_.user : "default";
  opts.password = *config_.password ? *config_.password : "";
  if (comp == 1) {
    chc_lz4_codec_init(&conn->codec);
    opts.compression = CHC_COMP_LZ4;
    opts.codec = &conn->codec;
  } else if (comp == 2) {
    chc_zstd_codec_init(&conn->codec);
    opts.compression = CHC_COMP_ZSTD;
    opts.codec = &conn->codec;
  } else {
    opts.compression = CHC_COMP_NONE;
    opts.codec = nullptr;
  }

  chc_err cerr;
  chc_err_reset(&cerr);
  if (chc_client_init(&conn->client, &opts, &conn->al, &conn->io, &cerr) !=
      CHC_OK) {
    err = std::string("ClickHouse handshake failed: ") + cerr.msg;
    return false;
  }

  conn_ = conn.release();
  return true;
}

namespace {

// Backing storage for one columnar block. The block builder does NOT copy the
// slabs we hand it (they "must outlive chc_block_write"), so every array we
// build here stays alive in this object until the whole block has been sent.
//
// Fixed columns: a contiguous little-endian value buffer (n_rows * elem_size).
// String columns: a data blob plus a cumulative-end offsets array.
// LowCardinality(String): a dictionary (offsets + blob) plus a keys array; the
// dictionary holds each distinct value once, keys index into it.
struct ColumnStore {
  // Fixed-width numeric columns, stored as raw LE bytes.
  std::vector<std::vector<uint8_t>> fixed_data;
  // Plain string columns: data blob + cumulative offsets.
  std::vector<std::vector<uint8_t>> str_data;
  std::vector<std::vector<uint64_t>> str_offsets;
  // LowCardinality string columns.
  std::vector<std::vector<uint8_t>> lc_dict_data;
  std::vector<std::vector<uint64_t>> lc_dict_offsets;
  std::vector<std::vector<uint32_t>> lc_keys; // key_size = 4 (UInt32)
};

// Append `v`'s little-endian bytes to `out`. T must be a trivially-copyable
// arithmetic type; the platform is assumed little-endian (x86_64 / arm64), the
// only platforms this extension targets.
template <typename T> void put_le(std::vector<uint8_t> &out, T v) {
  const auto *p = reinterpret_cast<const uint8_t *>(&v);
  out.insert(out.end(), p, p + sizeof(T));
}

// Build a plain-String column's (data, offsets) from a per-row accessor.
template <typename Fn>
void build_string_col(const std::vector<EventRow> &batch, ColumnStore &cs,
                      size_t &data_idx, size_t &off_idx, Fn get) {
  cs.str_data.emplace_back();
  cs.str_offsets.emplace_back();
  std::vector<uint8_t> &data = cs.str_data.back();
  std::vector<uint64_t> &offs = cs.str_offsets.back();
  offs.reserve(batch.size());
  for (const EventRow &r : batch) {
    const std::string &s = get(r);
    data.insert(data.end(), s.begin(), s.end());
    offs.push_back(data.size()); // cumulative exclusive end
  }
  data_idx = cs.str_data.size() - 1;
  off_idx = cs.str_offsets.size() - 1;
}

// Build a non-Nullable LowCardinality(String) column: dedup values into a
// dictionary and emit a UInt32 key per row. Unlike the Nullable variant, index
// 0 is a real value (no empty-string sentinel) -- the first distinct value
// seen takes key 0. (The Nullable convention of a slot-0 null sentinel does not
// apply here; our columns are non-Nullable, so an empty query field like
// client_ip is just a normal dictionary entry.)
template <typename Fn>
void build_lc_col(const std::vector<EventRow> &batch, ColumnStore &cs,
                  size_t &keys_idx, size_t &dict_data_idx, size_t &dict_off_idx,
                  size_t &dict_n, Fn get) {
  cs.lc_keys.emplace_back();
  cs.lc_dict_data.emplace_back();
  cs.lc_dict_offsets.emplace_back();
  std::vector<uint32_t> &keys = cs.lc_keys.back();
  std::vector<uint8_t> &dict_data = cs.lc_dict_data.back();
  std::vector<uint64_t> &dict_offs = cs.lc_dict_offsets.back();
  keys.reserve(batch.size());

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

  keys_idx = cs.lc_keys.size() - 1;
  dict_data_idx = cs.lc_dict_data.size() - 1;
  dict_off_idx = cs.lc_dict_offsets.size() - 1;
  dict_n = dict_offs.size();
}

// A parsed chc_type paired with its allocator, destroyed on scope exit.
struct ScopedType {
  chc_type *t = nullptr;
  const chc_alloc *al = nullptr;
  ~ScopedType() {
    if (t)
      chc_type_destroy(t, al);
  }
};

bool parse_type(const chc_alloc *al, const char *name, ScopedType &out,
                std::string &err) {
  chc_err cerr;
  chc_err_reset(&cerr);
  out.al = al;
  if (chc_type_parse(name, strlen(name), al, &out.t, &cerr) != CHC_OK) {
    err = std::string("chc_type_parse(") + name + "): " + cerr.msg;
    return false;
  }
  return true;
}

} // namespace

bool ChNativeSink::flush(const std::vector<EventRow> &batch, std::string &err) {
  if (batch.empty())
    return true;
  if (cfg(config_.host).empty()) {
    err = "host not configured";
    return false;
  }
  if (!ensure_connected(err))
    return false;

  const std::string database =
      cfg(config_.database).empty() ? "default" : cfg(config_.database);
  const std::string table =
      cfg(config_.table).empty() ? "events_raw" : cfg(config_.table);
  const size_t n = batch.size();
  const chc_alloc *al = &conn_->al;

  chc_err cerr;
  chc_err_reset(&cerr);

  // Start the INSERT: the server replies with a header (empty) Data block
  // describing the target columns, which we drain before sending our block.
  //
  // The column list MUST name every column we append to the block below (see
  // the add_str/add_lc/add_fixed calls), in the same order and with matching
  // types. ClickHouse maps the sent block against the columns named here; a
  // column we append but do not name is dropped (silently lands as the column
  // default), and a name/type/order mismatch corrupts or rejects the insert.
  // This list, the add_* sequence, and the events_raw schema are one contract.
  const std::string insert =
      "INSERT INTO " + database + "." + table +
      " (event_time, user, client_ip, schema, sql_command, connection_id, "
      "in_transaction, query_time_secs, lock_time_secs, "
      "rows_sent, rows_examined, rows_affected, warning_count, status, "
      "digest_text, digest_hash, query, sqlstate, error_message, port, "
      "bytes_sent, "
      "bytes_received, select_full_join, select_full_range_join, select_range, "
      "select_range_check, select_scan, sort_merge_passes, sort_range, "
      "sort_rows, sort_scan, created_tmp_tables, created_tmp_disk_tables, "
      "no_index_used, no_good_index_used, read_first, read_last, read_key, "
      "read_next, read_prev, read_rnd, read_rnd_next) VALUES";
  if (chc_client_send_query(conn_->client, insert.c_str(), insert.size(),
                            nullptr, 0, &cerr) != CHC_OK) {
    err = std::string("send_query failed: ") + cerr.msg;
    disconnect();
    return false;
  }

  // Drain the server's response until we get the header Data block (or an
  // exception). recv_packet returns CHC_OK for exception packets too.
  bool got_header = false;
  while (!got_header) {
    chc_packet pkt;
    memset(&pkt, 0, sizeof(pkt));
    if (chc_client_recv_packet(conn_->client, &pkt, &cerr) != CHC_OK) {
      err = std::string("recv (header) failed: ") + cerr.msg;
      disconnect();
      return false;
    }
    if (pkt.kind == CHC_PKT_EXCEPTION) {
      err = std::string("server rejected INSERT: ") +
            (pkt.exception ? "server exception" : "unknown");
      chc_packet_clear(conn_->client, &pkt);
      disconnect();
      return false;
    }
    if (pkt.kind == CHC_PKT_DATA)
      got_header = true;
    chc_packet_clear(conn_->client, &pkt);
  }

  // Transpose the row batch into columns.
  ColumnStore cs;
  size_t di = 0, oi = 0; // reused string-column indices
  size_t ki = 0, ddi = 0, doi = 0, dn = 0;

  // LowCardinality string columns.
  size_t lc_user_k, lc_user_dd, lc_user_do, lc_user_n;
  build_lc_col(batch, cs, lc_user_k, lc_user_dd, lc_user_do, lc_user_n,
               [](const EventRow &r) -> const std::string & { return r.user; });
  size_t lc_ip_k, lc_ip_dd, lc_ip_do, lc_ip_n;
  build_lc_col(
      batch, cs, lc_ip_k, lc_ip_dd, lc_ip_do, lc_ip_n,
      [](const EventRow &r) -> const std::string & { return r.client_ip; });
  size_t lc_schema_k, lc_schema_dd, lc_schema_do, lc_schema_n;
  build_lc_col(
      batch, cs, lc_schema_k, lc_schema_dd, lc_schema_do, lc_schema_n,
      [](const EventRow &r) -> const std::string & { return r.schema; });
  size_t lc_cmd_k, lc_cmd_dd, lc_cmd_do, lc_cmd_n;
  build_lc_col(
      batch, cs, lc_cmd_k, lc_cmd_dd, lc_cmd_do, lc_cmd_n,
      [](const EventRow &r) -> const std::string & { return r.sql_command; });
  // sqlstate is LowCardinality(String) in the schema: a 5-char SQLSTATE has few
  // distinct values, so it dictionary-encodes well.
  size_t lc_ss_k, lc_ss_dd, lc_ss_do, lc_ss_n;
  build_lc_col(
      batch, cs, lc_ss_k, lc_ss_dd, lc_ss_do, lc_ss_n,
      [](const EventRow &r) -> const std::string & { return r.sqlstate; });

  // Plain String columns: query, digest_text, digest_hash, error_message.
  size_t q_d, q_o;
  build_string_col(
      batch, cs, q_d, q_o,
      [](const EventRow &r) -> const std::string & { return r.query; });
  size_t dg_d, dg_o;
  build_string_col(
      batch, cs, dg_d, dg_o,
      [](const EventRow &r) -> const std::string & { return r.digest_text; });
  size_t dh_d, dh_o;
  build_string_col(
      batch, cs, dh_d, dh_o,
      [](const EventRow &r) -> const std::string & { return r.digest_hash; });
  size_t em_d, em_o;
  build_string_col(
      batch, cs, em_d, em_o,
      [](const EventRow &r) -> const std::string & { return r.error_message; });

  // Fixed numeric columns, each a contiguous LE buffer.
  auto fixed_col = [&](auto get) -> size_t {
    cs.fixed_data.emplace_back();
    std::vector<uint8_t> &buf = cs.fixed_data.back();
    for (const EventRow &r : batch)
      put_le(buf, get(r));
    return cs.fixed_data.size() - 1;
  };
  const size_t f_conn =
      fixed_col([](const EventRow &r) { return r.connection_id; });
  const size_t f_txn = fixed_col(
      [](const EventRow &r) -> uint8_t { return r.in_transaction ? 1 : 0; });
  // event_time is DateTime64(6): an Int64 count of microsecond ticks since
  // epoch. query_start_utime is already microseconds since epoch, so the value
  // is sent unchanged (DateTime64(6) tick == 1 microsecond).
  const size_t f_start =
      fixed_col([](const EventRow &r) { return r.query_start_utime; });
  const size_t f_qtime =
      fixed_col([](const EventRow &r) { return r.query_time_secs; });
  const size_t f_ltime =
      fixed_col([](const EventRow &r) { return r.lock_time_secs; });
  const size_t f_sent =
      fixed_col([](const EventRow &r) { return r.rows_sent; });
  const size_t f_exam =
      fixed_col([](const EventRow &r) { return r.rows_examined; });
  const size_t f_aff =
      fixed_col([](const EventRow &r) { return r.rows_affected; });
  // warning_count is UInt32 in the schema (Arnaud's DDL); narrow on emit.
  const size_t f_warn = fixed_col(
      [](const EventRow &r) -> uint32_t {
        return static_cast<uint32_t>(r.warning_count);
      });
  // status is UInt16 (the MySQL error code fits; 0 on success).
  const size_t f_status =
      fixed_col([](const EventRow &r) -> uint16_t {
        return static_cast<uint16_t>(r.status);
      });
  const size_t f_port =
      fixed_col([](const EventRow &r) -> uint16_t { return r.port; });
  const size_t f_bsent =
      fixed_col([](const EventRow &r) { return r.bytes_sent; });
  const size_t f_brecv =
      fixed_col([](const EventRow &r) { return r.bytes_received; });
  // The optimizer/sort/tmp counters are UInt32 in the schema (they are small
  // per-statement deltas); narrow each on emit. sort_rows stays UInt64.
  const size_t f_sfj = fixed_col([](const EventRow &r) -> uint32_t {
    return static_cast<uint32_t>(r.select_full_join);
  });
  const size_t f_sfrj = fixed_col([](const EventRow &r) -> uint32_t {
    return static_cast<uint32_t>(r.select_full_range_join);
  });
  const size_t f_srng = fixed_col([](const EventRow &r) -> uint32_t {
    return static_cast<uint32_t>(r.select_range);
  });
  const size_t f_src = fixed_col([](const EventRow &r) -> uint32_t {
    return static_cast<uint32_t>(r.select_range_check);
  });
  const size_t f_sscan = fixed_col([](const EventRow &r) -> uint32_t {
    return static_cast<uint32_t>(r.select_scan);
  });
  const size_t f_smp = fixed_col([](const EventRow &r) -> uint32_t {
    return static_cast<uint32_t>(r.sort_merge_passes);
  });
  const size_t f_sortr = fixed_col([](const EventRow &r) -> uint32_t {
    return static_cast<uint32_t>(r.sort_range);
  });
  const size_t f_sortrows =
      fixed_col([](const EventRow &r) { return r.sort_rows; });
  const size_t f_sortscan = fixed_col([](const EventRow &r) -> uint32_t {
    return static_cast<uint32_t>(r.sort_scan);
  });
  const size_t f_ctt = fixed_col([](const EventRow &r) -> uint32_t {
    return static_cast<uint32_t>(r.created_tmp_tables);
  });
  const size_t f_ctdt = fixed_col([](const EventRow &r) -> uint32_t {
    return static_cast<uint32_t>(r.created_tmp_disk_tables);
  });
  const size_t f_niu = fixed_col(
      [](const EventRow &r) -> uint8_t { return r.no_index_used ? 1 : 0; });
  const size_t f_ngiu = fixed_col([](const EventRow &r) -> uint8_t {
    return r.no_good_index_used ? 1 : 0;
  });
  // Handler row-access counters (UInt64 -- row-read counts can be large).
  const size_t f_rfirst =
      fixed_col([](const EventRow &r) { return r.read_first; });
  const size_t f_rlast =
      fixed_col([](const EventRow &r) { return r.read_last; });
  const size_t f_rkey = fixed_col([](const EventRow &r) { return r.read_key; });
  const size_t f_rnext =
      fixed_col([](const EventRow &r) { return r.read_next; });
  const size_t f_rprev =
      fixed_col([](const EventRow &r) { return r.read_prev; });
  const size_t f_rrnd = fixed_col([](const EventRow &r) { return r.read_rnd; });
  const size_t f_rrndnext =
      fixed_col([](const EventRow &r) { return r.read_rnd_next; });

  // Parse the column types once for this block. These match the fixed
  // events_raw schema (see README / ch_native_live.test): event_time is
  // DateTime64(6), the flag columns are Bool, the small per-statement counters
  // are UInt32, and sqlstate is LowCardinality(String).
  ScopedType t_lc, t_string, t_u64, t_u32, t_u16, t_bool, t_f64, t_dt64;
  if (!parse_type(al, "LowCardinality(String)", t_lc, err) ||
      !parse_type(al, "String", t_string, err) ||
      !parse_type(al, "UInt64", t_u64, err) ||
      !parse_type(al, "UInt32", t_u32, err) ||
      !parse_type(al, "UInt16", t_u16, err) ||
      !parse_type(al, "Bool", t_bool, err) ||
      !parse_type(al, "Float64", t_f64, err) ||
      !parse_type(al, "DateTime64(6)", t_dt64, err)) {
    disconnect();
    return false;
  }

  chc_block_builder *bb = nullptr;
  if (chc_block_builder_init(&bb, al, &cerr) != CHC_OK) {
    err = std::string("block_builder_init: ") + cerr.msg;
    disconnect();
    return false;
  }

  bool ok = true;
  auto add_lc = [&](const char *name, size_t k, size_t dd, size_t doff,
                    size_t dnn) {
    if (!ok)
      return;
    ok = chc_block_builder_append_low_cardinality_string(
             bb, name, strlen(name), t_lc.t, /*key_size=*/4,
             cs.lc_keys[k].data(), cs.lc_dict_offsets[doff].data(),
             cs.lc_dict_data[dd].data(), dnn, n, &cerr) == CHC_OK;
  };
  auto add_str = [&](const char *name, size_t d, size_t o) {
    if (!ok)
      return;
    ok = chc_block_builder_append_string(
             bb, name, strlen(name), cs.str_offsets[o].data(),
             cs.str_data[d].data(), n, &cerr) == CHC_OK;
  };
  auto add_fixed = [&](const char *name, const chc_type *t, size_t f) {
    if (!ok)
      return;
    ok = chc_block_builder_append_fixed(bb, name, strlen(name), t,
                                        cs.fixed_data[f].data(), n,
                                        &cerr) == CHC_OK;
  };

  // Column order MUST match the INSERT list above and the events_raw schema.
  add_fixed("event_time", t_dt64.t, f_start);
  add_lc("user", lc_user_k, lc_user_dd, lc_user_do, lc_user_n);
  add_lc("client_ip", lc_ip_k, lc_ip_dd, lc_ip_do, lc_ip_n);
  add_lc("schema", lc_schema_k, lc_schema_dd, lc_schema_do, lc_schema_n);
  add_lc("sql_command", lc_cmd_k, lc_cmd_dd, lc_cmd_do, lc_cmd_n);
  add_fixed("connection_id", t_u64.t, f_conn);
  add_fixed("in_transaction", t_bool.t, f_txn);
  add_fixed("query_time_secs", t_f64.t, f_qtime);
  add_fixed("lock_time_secs", t_f64.t, f_ltime);
  add_fixed("rows_sent", t_u64.t, f_sent);
  add_fixed("rows_examined", t_u64.t, f_exam);
  add_fixed("rows_affected", t_u64.t, f_aff);
  add_fixed("warning_count", t_u32.t, f_warn);
  add_fixed("status", t_u16.t, f_status);
  add_str("digest_text", dg_d, dg_o);
  add_str("digest_hash", dh_d, dh_o);
  add_str("query", q_d, q_o);
  add_lc("sqlstate", lc_ss_k, lc_ss_dd, lc_ss_do, lc_ss_n);
  add_str("error_message", em_d, em_o);
  add_fixed("port", t_u16.t, f_port);
  add_fixed("bytes_sent", t_u64.t, f_bsent);
  add_fixed("bytes_received", t_u64.t, f_brecv);
  add_fixed("select_full_join", t_u32.t, f_sfj);
  add_fixed("select_full_range_join", t_u32.t, f_sfrj);
  add_fixed("select_range", t_u32.t, f_srng);
  add_fixed("select_range_check", t_u32.t, f_src);
  add_fixed("select_scan", t_u32.t, f_sscan);
  add_fixed("sort_merge_passes", t_u32.t, f_smp);
  add_fixed("sort_range", t_u32.t, f_sortr);
  add_fixed("sort_rows", t_u64.t, f_sortrows);
  add_fixed("sort_scan", t_u32.t, f_sortscan);
  add_fixed("created_tmp_tables", t_u32.t, f_ctt);
  add_fixed("created_tmp_disk_tables", t_u32.t, f_ctdt);
  add_fixed("no_index_used", t_bool.t, f_niu);
  add_fixed("no_good_index_used", t_bool.t, f_ngiu);
  add_fixed("read_first", t_u64.t, f_rfirst);
  add_fixed("read_last", t_u64.t, f_rlast);
  add_fixed("read_key", t_u64.t, f_rkey);
  add_fixed("read_next", t_u64.t, f_rnext);
  add_fixed("read_prev", t_u64.t, f_rprev);
  add_fixed("read_rnd", t_u64.t, f_rrnd);
  add_fixed("read_rnd_next", t_u64.t, f_rrndnext);

  // Silence unused-index warnings from the reused scratch variables.
  (void)di;
  (void)oi;
  (void)ki;
  (void)ddi;
  (void)doi;
  (void)dn;

  if (!ok) {
    err = std::string("append column failed: ") + cerr.msg;
    chc_block_builder_destroy(bb);
    disconnect();
    return false;
  }

  // Send our data block, then the empty terminator block that ends the INSERT
  // stream.
  if (chc_client_send_data(conn_->client, bb, &cerr) != CHC_OK) {
    err = std::string("send_data failed: ") + cerr.msg;
    chc_block_builder_destroy(bb);
    disconnect();
    return false;
  }
  chc_block_builder_destroy(bb);

  if (chc_client_send_data(conn_->client, nullptr, &cerr) != CHC_OK) {
    err = std::string("send_data (terminator) failed: ") + cerr.msg;
    disconnect();
    return false;
  }

  // Drain until END_OF_STREAM; surface a server exception as a flush error.
  for (;;) {
    chc_packet pkt;
    memset(&pkt, 0, sizeof(pkt));
    if (chc_client_recv_packet(conn_->client, &pkt, &cerr) != CHC_OK) {
      err = std::string("recv (commit) failed: ") + cerr.msg;
      disconnect();
      return false;
    }
    const chc_packet_kind kind = pkt.kind;
    const bool is_exception = kind == CHC_PKT_EXCEPTION;
    chc_packet_clear(conn_->client, &pkt);
    if (is_exception) {
      err = "server exception during INSERT commit";
      disconnect();
      return false;
    }
    if (kind == CHC_PKT_END_OF_STREAM)
      break;
  }

  return true;
}

} // namespace vsql_stat_ch_native
