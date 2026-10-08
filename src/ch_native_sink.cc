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

#include "event_block.h"

namespace vsql_stat_ch_native {

namespace {

using ::vsql_stat::EventRow;
using ::vsql_stat_ch::EventBlock;

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
  // A server-side handshake rejection comes back as an exception object (with
  // cerr.msg left empty) rather than as text in cerr; surface its message so
  // the error reads the same as before.
  chc_exception *exc = nullptr;
  if (chc_client_init(&conn->client, &opts, &conn->al, &conn->io, &exc,
                      &cerr) != CHC_OK) {
    err = std::string("ClickHouse handshake failed: ") + cerr.msg;
    if (exc != nullptr) {
      if (cerr.msg[0] == '\0' && exc->display_text != nullptr)
        err.append(exc->display_text, exc->display_text_len);
      chc_exception_free(exc, &conn->al);
    }
    return false;
  }

  conn_ = conn.release();
  return true;
}

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
  const chc_alloc *al = &conn_->al;

  chc_err cerr;
  chc_err_reset(&cerr);

  // Start the INSERT: the server replies with a header (empty) Data block
  // describing the target columns, which we drain before sending our block.
  //
  // The column list MUST name every column we append to the block below (see
  // EventBlock::build in event_block.cc), in the same order and with matching
  // types. ClickHouse maps the sent block against the columns named here; a
  // column we append but do not name is dropped (silently lands as the column
  // default), and a name/type/order mismatch corrupts or rejects the insert.
  // This list, EventBlock::build, and the events_raw schema are one contract.
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
      "read_next, read_prev, read_rnd, read_rnd_next, "
      "client_pid, client_name, program_name) VALUES";
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

  // Transpose the row batch into columns (shared with the HTTP transport).
  EventBlock block(al);
  if (!block.build(batch, EventBlock::Options{}, err)) {
    disconnect();
    return false;
  }

  // Send our data block, then the empty terminator block that ends the INSERT
  // stream.
  if (chc_client_send_data(conn_->client, block.builder(), &cerr) != CHC_OK) {
    err = std::string("send_data failed: ") + cerr.msg;
    disconnect();
    return false;
  }

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
