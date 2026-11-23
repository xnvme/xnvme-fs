// SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "xnvme_fs.hpp"
#include "duckdb/common/common.hpp"

#include <memory>

namespace duckdb {
namespace xnvme_fs {

//! Whether writes to 'path' may return before they reach the drive: DuckDB's temporary files, which it never syncs.
bool WritesBehind(const string &path);

//! Starts writing a copy of 'buffer'. It reaches the drive later and is reaped by this thread, which until then
//! holds the file open, so its extents stay its own even if the file is removed. A later write to its range before
//! then is copied into it, and it is written again once it lands; if this thread exits first, the next thread to
//! reap writes it.
void WriteBehind(const std::shared_ptr<::xnvme::fs::File> &file, const void *buffer, idx_t nbytes, idx_t offset);

//! Reaps what this thread wrote behind, all of it with 'wait'; throws an IOException for a write that failed.
void Reap(bool wait);

//! Bytes of writes behind to one file still in flight, taken before a read and laid over what it read, so the read
//! returns them whether or not they reached the drive in between.
class Overlay {
public:
	Overlay(const ::xnvme::fs::File &file, idx_t nbytes, idx_t offset);
	void Apply(void *buffer) const;

private:
	struct Piece {
		idx_t at;
		vector<uint8_t> bytes;
	};
	vector<Piece> pieces;
};

//! Whether a write behind of any thread to 'file' reaches past 'size'.
bool InFlightPast(const ::xnvme::fs::File &file, idx_t size);

} // namespace xnvme_fs
} // namespace duckdb
