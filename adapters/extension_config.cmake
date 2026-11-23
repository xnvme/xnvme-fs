# SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
# SPDX-FileCopyrightText: Stichting DuckDB Foundation
#
# SPDX-License-Identifier: MIT

# This file is included by DuckDB's build system. It specifies which extension to load.
duckdb_extension_load(xnvme_fs
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
)

# TPC-H's generator and reference answers, for the SF1 test on a pool.
duckdb_extension_load(tpch)

# Since DuckDB 2.0, loading an extension here only builds it; the shell and the tests link the ones named here
duckdb_extension_statically_link(xnvme_fs tpch)
