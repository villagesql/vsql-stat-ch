-- Canonical ClickHouse schema for the vsql_stat_ch statement-telemetry sink.
--
-- This is the single source of truth for the target table. It is:
--   * generated per-test by CMake (table name substituted) into build/schema/
--     and curled into ClickHouse by the live MTR tests (ch_native_live,
--     http_transport_live) as their table setup;
--   * linked (not copied) from README.md, which points operators here;
--   * the shape the native sink's block builder (src/ch_native_sink.cc) writes.
--     The column NAMES, TYPES, and ORDER below are a contract with that builder.
--
-- The sink only INSERTs; operators create this table. Physical properties
-- (ENGINE, ORDER BY, PARTITION BY, TTL, replication) are yours to tune -- the
-- column definitions are the contract; everything after the closing paren is a
-- reasonable default you can change.
CREATE TABLE default.events_raw
(
    event_time         DateTime64(6)          COMMENT 'Statement start time (microsecond precision).',
    user               LowCardinality(String) COMMENT 'Authenticated user (priv_user).',
    client_ip          LowCardinality(String) COMMENT 'Client IP; empty for socket/pipe connections.',
    schema             LowCardinality(String) COMMENT 'Default database at execution time.',
    sql_command        LowCardinality(String) COMMENT 'SQL command class, e.g. SELECT, INSERT.',
    connection_id      UInt64                 COMMENT 'Server connection/thread id.',
    in_transaction     Bool                   COMMENT 'Statement ran inside an open transaction.',
    query_time_secs    Float64                COMMENT 'Wall-clock execution time in seconds.',
    lock_time_secs     Float64                COMMENT 'Time spent waiting on locks, in seconds.',
    rows_sent          UInt64                 COMMENT 'Rows returned to the client.',
    rows_examined      UInt64                 COMMENT 'Rows read to satisfy the statement.',
    rows_affected      UInt64                 COMMENT 'Rows changed (DML), 0 otherwise.',
    warning_count      UInt32                 COMMENT 'Warnings raised by the statement.',
    status             UInt16                 COMMENT 'MySQL error code; 0 on success.',
    digest_text        String                 COMMENT 'Normalized statement digest (literals stripped).',
    digest_hash        String                 COMMENT 'Statement identity hash (64-char hex; == performance_schema DIGEST).',
    query              String                 COMMENT 'Statement text (rewritten/redacted when applicable).',
    sqlstate           LowCardinality(String) COMMENT '5-char SQLSTATE; empty on success.',
    error_message      String DEFAULT ''      COMMENT 'Error text; empty on success.',
    port               UInt16                 COMMENT 'Client TCP port; 0 for socket connections.',
    bytes_sent         UInt64                 COMMENT 'Bytes sent to the client for this statement.',
    bytes_received     UInt64                 COMMENT 'Bytes received from the client for this statement.',
    select_full_join       UInt32             COMMENT 'Joins with no usable index.',
    select_full_range_join UInt32             COMMENT 'Joins using a range on a reference table.',
    select_range           UInt32             COMMENT 'Range scans on the first table.',
    select_range_check     UInt32             COMMENT 'Joins re-checking a key per row (no index).',
    select_scan            UInt32             COMMENT 'Full scans of the first table.',
    sort_merge_passes  UInt32                 COMMENT 'Sort merge passes (high = large sort).',
    sort_range         UInt32                 COMMENT 'Sorts done over a range.',
    sort_rows          UInt64                 COMMENT 'Rows sorted.',
    sort_scan          UInt32                 COMMENT 'Sorts done over a full scan.',
    created_tmp_tables      UInt32            COMMENT 'Internal temp tables created.',
    created_tmp_disk_tables UInt32            COMMENT 'Internal temp tables spilled to disk.',
    no_index_used      Bool                   COMMENT 'Statement ran without any index.',
    no_good_index_used Bool                   COMMENT 'No good index was available.',
    read_first         UInt64                 COMMENT 'Handler index-first reads (MIN / index start).',
    read_last          UInt64                 COMMENT 'Handler index-last reads (MAX / index end).',
    read_key           UInt64                 COMMENT 'Handler reads via an index lookup.',
    read_next          UInt64                 COMMENT 'Handler forward index walks (range scan).',
    read_prev          UInt64                 COMMENT 'Handler backward index walks (reverse scan).',
    read_rnd           UInt64                 COMMENT 'Handler reads by position (post-filesort fetch).',
    read_rnd_next      UInt64                 COMMENT 'Handler next-row in a full scan (high = table scan).'
)
ENGINE = MergeTree
PARTITION BY toDate(event_time)
ORDER BY (event_time)
TTL toDateTime(event_time) + INTERVAL 90 DAY DELETE,
    toDateTime(event_time) + INTERVAL 1 DAY RECOMPRESS CODEC(ZSTD(8))
COMMENT 'Per-statement telemetry from VillageSQL, one row per executed statement, exported by the vsql_stat_ch extension.';
