#!/usr/bin/env bash
# Copyright (c) 2026 VillageSQL Contributors
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2.0,
# as published by the Free Software Foundation.
#
# Local test runner: builds the VEB (via build.sh), stages it into a temporary
# veb-dir, and runs the repo's MTR suite against the dev server. Running MTR by
# hand in the dev environment is fiddly for this extension (it needs the VEB
# staged and, for the live test, a reachable ClickHouse), so this wraps it.
#
# Usage:
#   export VillageSQL_BUILD_DIR=$HOME/build/villagesql   # a from-source build tree
#   #   -- OR a prebuilt install (install.villagesql.com, INSTALL_METHOD=prebuilt):
#   export VillageSQL_BUILD_DIR=$HOME/.villagesql/prebuilt
#   ./test.sh                        # build + run the whole MTR suite
#   ./test.sh ch_native              # build + run a single test by name
#   ./test.sh ch_native http_transport   # build + run several named tests
#   RECORD=1 ./test.sh ch_native     # build + record one test's .result
#
# VillageSQL_BUILD_DIR may point at either a from-source build tree or a prebuilt
# install; both carry villagesql-extension-sdk-*/, mysql-test/, and bin/mysqld at
# the same relative paths, so build + MTR work against either.
#
# Any positional arguments are passed through to mysql-test-run.pl as test
# names; with none, the whole suite runs.

set -euo pipefail

# The one VEB this repo produces.
VEB="vsql_stat_ch.veb"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VILLAGESQL_BUILD_DIR="${VillageSQL_BUILD_DIR:-${VILLAGESQL_BUILD_DIR:-}}"

# Default to the prebuilt install when unset -- install.villagesql.com
# (INSTALL_METHOD=prebuilt) always lands the server+SDK at ~/.villagesql/prebuilt,
# so a vanilla install runs with zero config. Only if it actually exists.
# Export it so the build.sh child uses the same resolved value.
if [[ -z "$VILLAGESQL_BUILD_DIR" && -d "${HOME}/.villagesql/prebuilt" ]]; then
    VILLAGESQL_BUILD_DIR="${HOME}/.villagesql/prebuilt"
fi
export VillageSQL_BUILD_DIR="$VILLAGESQL_BUILD_DIR"

VEB_DIR="/tmp/${VEB%.veb}-test-veb-$$"

log() { echo "[test] $(date '+%H:%M:%S') - $1"; }

# The staged VEB dir is removed on exit. But with --start-and-exit MTR leaves
# the server running (with --veb-dir pointing here) and this script returns
# immediately, so the trap would delete the VEB out from under the live server.
# Set KEEP_TMP=1 to skip cleanup and keep the staged VEB dir; the path is
# printed so it can be removed by hand later.
cleanup() {
    if [[ "${KEEP_TMP:-0}" == "1" ]]; then
        log "KEEP_TMP=1 -- leaving staged VEB dir: $VEB_DIR"
        return
    fi
    rm -rf "$VEB_DIR" 2>/dev/null || true
}
trap cleanup EXIT

if [[ -z "$VILLAGESQL_BUILD_DIR" ]]; then
    log "ERROR: VillageSQL_BUILD_DIR is not set and no prebuilt install was found"
    log "  at \$HOME/.villagesql/prebuilt. Set one of:"
    log "  build tree:       export VillageSQL_BUILD_DIR=/path/to/villagesql/build"
    log "  prebuilt install: export VillageSQL_BUILD_DIR=\$HOME/.villagesql/prebuilt"
    log "                    (from install.villagesql.com, INSTALL_METHOD=prebuilt)"
    exit 1
fi

# Step 1: Build (delegates to build.sh so build logic lives in one place).
log "Building..."
"$SCRIPT_DIR/build.sh"

if [[ ! -f "$SCRIPT_DIR/build/$VEB" ]]; then
    log "ERROR: $VEB not found after build"
    exit 1
fi

# Step 2: Stage the VEB.
mkdir -p "$VEB_DIR"
cp "$SCRIPT_DIR/build/$VEB" "$VEB_DIR/"
log "VEB staged in $VEB_DIR"

# The live tests curl their table DDL rather than inlining it. CMake generates
# a per-test .sql (table name substituted) from the one canonical schema
# (schema/events_raw.sql) into build/schema/. Pass that dir so the
# tests need not guess it. A schema edit reaches the tests via the next build.
export VSQL_STAT_CH_SCHEMA_DIR="$SCRIPT_DIR/build/schema"

# Step 3: The live test skips unless VSQL_STAT_CH_URL points at a reachable
# ClickHouse. Auto-detect a local instance (e.g. the vsql-ch docker container);
# otherwise the live test skips and the suite stays hermetic.
CH_URL="${VSQL_STAT_CH_URL:-http://127.0.0.1:8123}"
if curl -sf "$CH_URL/ping" >/dev/null 2>&1; then
    log "ClickHouse reachable at $CH_URL -- live test will run"
    export VSQL_STAT_CH_URL="$CH_URL"
else
    log "No ClickHouse at $CH_URL -- live test will skip"
    unset VSQL_STAT_CH_URL
fi

# Step 4: Run MTR from the dev server's mysql-test dir (MTR manages its own
# server; do NOT start one manually).
MTR_RECORD_ARG=()
if [[ "${RECORD:-0}" == "1" ]]; then
    log "RECORD=1 set -- recording expected results"
    MTR_RECORD_ARG=(--record)
fi

log "Running MTR..."
# Run from inside mysql-test/ (not the tree root) so mtr's relative Perl @INC
# (`lib`) resolves to mysql-test/lib where My/ConfigFactory.pm etc. live. This
# matches how the prebuilt install's `villagesql mysql-test` wrapper launches
# it, and works for both a build tree and a prebuilt install. All --suite and
# --veb-dir args below are absolute, so the CWD change does not affect them.
cd "$VILLAGESQL_BUILD_DIR/mysql-test"
perl mysql-test-run.pl \
    --suite="$SCRIPT_DIR/mysql-test" \
    --parallel=1 \
    --nounit-tests \
    --mysqld=--veb-dir="$VEB_DIR" \
    ${MTR_RECORD_ARG[@]+"${MTR_RECORD_ARG[@]}"} \
    "$@"

log "Done."
