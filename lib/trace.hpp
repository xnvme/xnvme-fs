// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
// SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
//
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cstdint>
#include <string>

namespace xnvme_fs {

//! Whether XNVME_FS_TRACE named a file to trace into; read once, on first use.
bool Tracing();
//! Appends one line for an operation an engine asked of the library, or one of the extent protocol's beneath it, to
//! the file XNVME_FS_TRACE names: seconds since the process's first line, its pid and thread, the operation, the name,
//! an offset or size, and the bytes moved. Several processes may share the file; a line is one append.
void TraceOp(const char *op, const std::string &name, uint64_t offset = 0, uint64_t nbytes = 0);

} // namespace xnvme_fs

//! Traces only when tracing, so an untraced run builds no arguments
#define XNVME_FS_TRACE(...)                                                                                             \
	do {                                                                                                               \
		if (::xnvme_fs::Tracing()) {                                                                                    \
			::xnvme_fs::TraceOp(__VA_ARGS__);                                                                           \
		}                                                                                                              \
	} while (0)
