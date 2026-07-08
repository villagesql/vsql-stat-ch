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

// The clickhouse-c client is a header-only library that compiles its
// implementation into exactly one translation unit that defines
// CHC_IMPLEMENTATION before including the headers. This is that translation
// unit; every other file includes the same headers WITHOUT the define and gets
// only declarations. Keeping the implementation (and its <lz4.h>/<zstd.h>
// pulls) in this C file isolates it from the C++ sink code.
//
// CHC_PROVIDE_STDLIB_ALLOC gives us chc_alloc_stdlib() (malloc/free arena).

#define CHC_PROVIDE_STDLIB_ALLOC
#define CHC_IMPLEMENTATION

#include "clickhouse.h"

#include "clickhouse-client.h"
#include "clickhouse-compression.h"
#include "clickhouse-posix-io.h"
