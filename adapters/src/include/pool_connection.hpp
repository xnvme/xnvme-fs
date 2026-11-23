// SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "xnvme_fs.hpp"
#include "duckdb.hpp"
#include "duckdb/common/file_opener.hpp"

namespace duckdb {
namespace xnvme_fs {

//! The process's session with a pool's server, made on first use. The opener carries xnvme_fs_socket_dir,
//! xnvme_fs_backend and xnvme_fs_homi_id; DuckDB's storage opens files without one, and those reuse the pool's session
//! or the defaults.
std::shared_ptr<::xnvme::fs::Session> GetSession(const string &pool, optional_ptr<FileOpener> opener);

//! A failed call of xnvme fs as the DuckDB exception DuckDB expects for its errno.
[[noreturn]] void Raise(const std::system_error &err);

//! The default of xnvme_fs_socket_dir: XNVME_FS_SOCKET_DIR, else /run/xnvme-fs.
string DefaultSocketDir();
//! The default of xnvme_fs_backend: XNVME_FS_BACKEND, else upcie.
string DefaultBackend();
//! The default of xnvme_fs_homi_id: XNVME_FS_HOMI_ID, else 0.
uint32_t DefaultHomiId();

} // namespace xnvme_fs
} // namespace duckdb
