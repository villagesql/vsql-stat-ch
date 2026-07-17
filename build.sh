#!/usr/bin/env bash
# Copyright (c) 2026 VillageSQL Contributors
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2.0,
# as published by the Free Software Foundation.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Ensure the git submodules (core = vsql-stat-core, third_party/clickhouse-c)
# are present and at the pin this checkout records. A plain `git clone` or
# `git pull` does NOT populate/advance submodules, so without this a build
# would either fail (empty core/) or silently compile a stale core. We fix it
# here rather than make the user remember `git submodule update`.
#
# Resilient by design:
#   - only acts when this is a git checkout AND git is available (the published
#     server image has no git; there the sources are expected to be present
#     already, e.g. from a recursive clone on the host or a release tarball);
#   - never clobbers in-progress work: if a submodule has uncommitted changes
#     (develop-in-place), it warns and leaves it alone.
ensure_submodules() {
  # Not a git checkout, or no git binary (e.g. inside the Docker image): just
  # verify the sources landed some other way; error clearly if they did not.
  if ! command -v git >/dev/null 2>&1 || \
     ! git -C "${SCRIPT_DIR}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    if [[ ! -f "${SCRIPT_DIR}/core/capture_pipeline.h" ]] || \
       [[ ! -f "${SCRIPT_DIR}/third_party/clickhouse-c/clickhouse.h" ]]; then
      echo "ERROR: submodule sources missing and git is unavailable to fetch them." >&2
      echo "  Re-clone with:  git clone --recurse-submodules <url>" >&2
      echo "  or copy in core/ and third_party/clickhouse-c/ before building." >&2
      exit 1
    fi
    return 0
  fi

  # git checkout: is any submodule out of sync with the recorded pin?
  # `git submodule status` prefixes a line with '-' (not initialized) or '+'
  # (checked out at a different commit than the pin).
  local status
  status="$(git -C "${SCRIPT_DIR}" submodule status --recursive 2>/dev/null || true)"
  if echo "${status}" | grep -qE '^[-+]'; then
    # Refuse to auto-update a submodule that has uncommitted local changes --
    # that is deliberate develop-in-place work we must not clobber.
    if ! git -C "${SCRIPT_DIR}" submodule foreach --quiet --recursive \
           'git diff --quiet && git diff --cached --quiet' 2>/dev/null; then
      echo "[build] WARNING: a submodule has uncommitted changes; leaving it as-is." >&2
      echo "[build]          (not running 'git submodule update' so your work is safe)" >&2
    else
      echo "[build] submodules out of sync with pin -- updating..." >&2
      git -C "${SCRIPT_DIR}" submodule update --init --recursive
    fi
  fi
}
ensure_submodules

# Default to the prebuilt install location when unset: install.villagesql.com
# (INSTALL_METHOD=prebuilt) always lands the server+SDK at ~/.villagesql/prebuilt,
# so a vanilla install needs no configuration. Only default if it actually
# exists; otherwise fall through to the error below rather than proceed blindly.
if [[ -z "${VillageSQL_BUILD_DIR:-}" && -d "${HOME}/.villagesql/prebuilt" ]]; then
  VillageSQL_BUILD_DIR="${HOME}/.villagesql/prebuilt"
  echo "[build] VillageSQL_BUILD_DIR unset -- defaulting to ${VillageSQL_BUILD_DIR}"
fi

if [[ -z "${VillageSQL_BUILD_DIR:-}" ]]; then
  echo "ERROR: VillageSQL_BUILD_DIR is not set and no prebuilt install was found" >&2
  echo "  at \$HOME/.villagesql/prebuilt. Set one of:" >&2
  echo "  A from-source build tree:  export VillageSQL_BUILD_DIR=/path/to/villagesql/build" >&2
  echo "  OR a prebuilt install:     export VillageSQL_BUILD_DIR=\$HOME/.villagesql/prebuilt" >&2
  echo "                             (from install.villagesql.com, INSTALL_METHOD=prebuilt)" >&2
  exit 1
fi

BUILD_DIR="${SCRIPT_DIR}/build"
mkdir -p "${BUILD_DIR}"

# Select the newest SDK by modification time (not alphabetical) to avoid
# version mismatch when multiple SDK versions exist in the directory. Works for
# both a build tree and a prebuilt install -- both hold villagesql-extension-sdk-*/.
SDK_DIR="$(ls -dt "${VillageSQL_BUILD_DIR}"/villagesql-extension-sdk-*/ 2>/dev/null | head -1)"
if [[ -z "${SDK_DIR}" ]]; then
  echo "ERROR: No villagesql-extension-sdk-* found in ${VillageSQL_BUILD_DIR}" >&2
  echo "  Expected a VillageSQL build tree or a prebuilt install there." >&2
  exit 1
fi
# Strip any trailing slash so cache comparisons are stable.
SDK_DIR="${SDK_DIR%/}"
echo "[build] selected SDK: ${SDK_DIR}"

# The SDK selection must win on EVERY build, not just the first configure.
# cmake caches VillageSQL_SDK_DIR, and FindVillageSQL.cmake also has a glob-based
# fallback (Method 1) keyed off VillageSQL_BUILD_DIR that can resolve a different
# (possibly stale) SDK. To make the freshly-selected SDK authoritative without
# any manual cache-clearing:
#   - if the cached SDK differs from what we just selected, wipe the cache so the
#     new selection can't be silently overridden by a frozen value;
#   - do NOT pass VillageSQL_BUILD_DIR, so FindVillageSQL's glob fallback stays
#     off and only our explicit SDK_DIR (Method 2) is used.
CACHE_FILE="${BUILD_DIR}/CMakeCache.txt"
if [[ -f "${CACHE_FILE}" ]]; then
  CACHED_SDK="$(sed -n 's/^VillageSQL_SDK_DIR:[^=]*=//p' "${CACHE_FILE}" | head -1)"
  CACHED_SDK="${CACHED_SDK%/}"
  if [[ "${CACHED_SDK}" != "${SDK_DIR}" ]]; then
    echo "[build] SDK changed (cached: '${CACHED_SDK:-none}') -- reconfiguring clean"
    rm -f "${CACHE_FILE}"
  fi
fi

# VillageSQL_BUILD_DIR may point at EITHER a from-source build tree OR a prebuilt
# install (e.g. ~/.villagesql/prebuilt from `install.villagesql.com`
# INSTALL_METHOD=prebuilt). Both carry villagesql-extension-sdk-*/, mysql-test/,
# and bin/mysqld at the same relative paths, so the SDK glob and the test run
# work for both. The only difference is veb_output_directory/ (where a build tree
# stages the server's own bundled extensions) -- a prebuilt install has none.
# Only pass VillageSQL_VEB_INSTALL_DIR when it exists; otherwise the VEB just
# lands in build/ (which is all test.sh needs -- it stages the VEB into a temp
# veb-dir itself).
VEB_INSTALL_ARGS=()
if [[ -d "${VillageSQL_BUILD_DIR}/veb_output_directory" ]]; then
  VEB_INSTALL_ARGS=(-DVillageSQL_VEB_INSTALL_DIR="${VillageSQL_BUILD_DIR}/veb_output_directory")
fi

# FORCE the cache value so a re-configure always adopts the current selection.
cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" \
  -DVillageSQL_SDK_DIR:PATH="${SDK_DIR}" \
  ${VEB_INSTALL_ARGS[@]+"${VEB_INSTALL_ARGS[@]}"}

cmake --build "${BUILD_DIR}" -- -j"$(sysctl -n hw.logicalcpu 2>/dev/null || nproc)"

echo ""
echo "Build complete. VEB file:"
ls -lh "${BUILD_DIR}/"*.veb 2>/dev/null || \
  find "${BUILD_DIR}" -name "*.veb" -print
