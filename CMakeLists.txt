# Copyright (c) 2026 VillageSQL Contributors
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, see <https://www.gnu.org/licenses/>.

cmake_minimum_required(VERSION 3.16)
project(vsql_stat_ch C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(VillageSQL_USE_DEV_HEADERS ON)
find_package(VillageSQL REQUIRED)

# The clickhouse-c native-protocol client is vendored as a git submodule
# (following the pg_stat_ch reference). Fail early and clearly if a caller
# cloned without --recurse-submodules / did not run submodule update --init.
set(CHC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/clickhouse-c)
if(NOT EXISTS ${CHC_DIR}/clickhouse.h)
  message(FATAL_ERROR
    "clickhouse-c submodule not found at ${CHC_DIR}.\n"
    "  Run: git submodule update --init "
    "third_party/clickhouse-c\n")
endif()

# clickhouse-c's LZ4/ZSTD codecs pull <lz4.h>/<zstd.h> and link -llz4 -lzstd.
find_package(PkgConfig)
if(PkgConfig_FOUND)
  pkg_check_modules(LZ4 liblz4)
  pkg_check_modules(ZSTD libzstd)
endif()
find_library(LZ4_LIB NAMES lz4 liblz4 HINTS ${LZ4_LIBRARY_DIRS})
find_library(ZSTD_LIB NAMES zstd libzstd HINTS ${ZSTD_LIBRARY_DIRS})
if(NOT LZ4_LIB OR NOT ZSTD_LIB)
  message(FATAL_ERROR
    "lz4 and zstd development libraries are required by the ClickHouse "
    "native sink (block compression).\n"
    "  Ubuntu/Debian: sudo apt install liblz4-dev libzstd-dev\n"
    "  RHEL/Fedora:   sudo dnf install lz4-devel libzstd-devel\n"
    "  macOS:         brew install lz4 zstd\n")
endif()

# libcurl for the HTTP transport (JSONEachRow POST).
find_package(CURL)
if(NOT CURL_FOUND)
  message(FATAL_ERROR
    "libcurl development headers not found (needed by the HTTP transport).\n"
    "  Ubuntu/Debian: sudo apt install libcurl4-openssl-dev\n"
    "  RHEL/Fedora:   sudo dnf install libcurl-devel\n"
    "  macOS:         included with Xcode Command Line Tools\n")
endif()

message(STATUS "VillageSQL SDK found at: ${VillageSQL_PREFIX}")
message(STATUS "clickhouse-c submodule: ${CHC_DIR}")
message(STATUS "lz4: ${LZ4_LIB}  zstd: ${ZSTD_LIB}  curl: ${CURL_VERSION_STRING}")

# The clickhouse-c implementation compiles into exactly one TU
# (clickhouse_c_impl.c defines CHC_IMPLEMENTATION); the C++ sink includes the
# same headers for declarations only.
add_library(vsql_stat_ch SHARED
    core/event_queue.cc
    core/capture_pipeline.cc
    src/extension.cc
    src/ch_native_sink.cc
    src/clickhouse_c_impl.c
    src/http_sink.cc
)
target_compile_options(vsql_stat_ch PRIVATE -fPIC)

# clickhouse-c is vendored third-party code: include it as SYSTEM so its
# warnings do not fail our (potentially -Werror) build.
target_include_directories(vsql_stat_ch SYSTEM PRIVATE ${CHC_DIR})
if(LZ4_INCLUDE_DIRS)
  target_include_directories(vsql_stat_ch SYSTEM PRIVATE ${LZ4_INCLUDE_DIRS})
endif()
if(ZSTD_INCLUDE_DIRS)
  target_include_directories(vsql_stat_ch SYSTEM PRIVATE ${ZSTD_INCLUDE_DIRS})
endif()

target_include_directories(vsql_stat_ch PRIVATE
    ${VillageSQL_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}       # so "core/..." resolves
    ${CMAKE_CURRENT_SOURCE_DIR}/src   # so "ch_native_sink.h" resolves
)
target_link_libraries(vsql_stat_ch PRIVATE ${LZ4_LIB} ${ZSTD_LIB} CURL::libcurl)

VEF_CREATE_VEB(
    NAME vsql_stat_ch
    LIBRARY_TARGET vsql_stat_ch
    MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)

# Single-source the ClickHouse schema. schema/events_raw.sql is the one
# canonical DDL: the artifact operators apply to ClickHouse (README points at
# it) AND the shape the native sink's block builder targets. The live MTR tests
# need the table under distinct names so they never collide; rather than inline
# the DDL in each test (which drifts), generate a per-test .sql here by
# substituting the table name. Tests curl the generated file (path via
# $VSQL_STAT_CH_SCHEMA_DIR, exported by test.sh). Regenerated on every
# configure, so a schema edit reaches the tests with no manual sync.
set(CH_SCHEMA_SRC ${CMAKE_CURRENT_SOURCE_DIR}/schema/events_raw.sql)
file(READ ${CH_SCHEMA_SRC} CH_SCHEMA_TEXT)
foreach(tbl events_raw_native events_raw_http)
  string(REPLACE "default.events_raw" "default.${tbl}" _ddl "${CH_SCHEMA_TEXT}")
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/schema/${tbl}.sql "${_ddl}")
endforeach()
message(STATUS "ClickHouse schema generated from: ${CH_SCHEMA_SRC}")
