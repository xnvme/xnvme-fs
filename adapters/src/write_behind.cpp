// SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
//
// SPDX-License-Identifier: MIT

#include "write_behind.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace duckdb {
namespace xnvme_fs {

namespace {
struct Behind {
	std::shared_ptr<::xnvme::fs::File> file;
	::xnvme_fs::req request = ::xnvme_fs::req_new();
	//! The errno of a write that could not be started, which is then done
	int unstarted = 0;
	idx_t offset = 0;
	vector<uint8_t> bytes;
	//! A later write to its range was copied into 'bytes' and may have landed first, so it is written again
	bool rewrite = false;
};

std::mutex registry_lock;
// By File::Id(), which every handle on one file shares
std::unordered_map<const void *, vector<Behind *>> in_flight;
// Writes to rewrite whose thread exited, for the next thread that reaps
vector<unique_ptr<Behind>> orphans;
std::atomic<bool> any_orphans {false};

void Unregister(Behind *b) {
	auto it = in_flight.find(b->file->Id());
	if (it == in_flight.end()) {
		return;
	}
	auto &list = it->second;
	list.erase(std::remove(list.begin(), list.end(), b), list.end());
	if (list.empty()) {
		in_flight.erase(it);
	}
}

//! Starts the write of 'b'. Never throws: a write that could not be started is done, and ReapList reports why.
void Start(Behind &b) {
	b.unstarted = 0;
	try {
		b.file->Handle().submit(b.request, 1, b.bytes.data(), b.bytes.size(), b.offset);
	} catch (std::system_error &err) {
		b.unstarted = err.code().value() ? err.code().value() : EIO;
	}
}

//! Submits a write again, with registry_lock held, so no later write changes its bytes while they are copied out.
void Again(Behind &b) {
	b.rewrite = false;
	Start(b);
}

//! Reaps pending writes; returns the errno of the first that failed, or 0. 'exiting' is a thread's exit, whose queues
//! may be gone already, so a write to rewrite is left to another thread.
int ReapList(vector<unique_ptr<Behind>> &list, bool wait, bool exiting = false) {
	int failed = 0;
	for (auto it = list.begin(); it != list.end();) {
		auto &b = **it;
		bool done = true;
		int status = b.unstarted;
		try {
			done = status || wait || b.request.poll();
			if (done && !status) {
				b.request.wait();
			}
		} catch (std::system_error &err) {
			status = err.code().value() ? err.code().value() : EIO;
		}
		if (!done) {
			++it;
			continue;
		}
		if (status && !failed) {
			failed = status;
		}
		{
			std::lock_guard<std::mutex> guard(registry_lock);
			if (b.rewrite && !status) {
				if (exiting) {
					orphans.push_back(std::move(*it));
					any_orphans = true;
					it = list.erase(it);
				} else {
					Again(b);
				}
				continue;
			}
			Unregister(&b);
		}
		it = list.erase(it);
	}
	return failed;
}

// A thread's queues are its own, so only it can reap what it submitted; at its exit it waits for the rest
struct Pending {
	vector<unique_ptr<Behind>> list;
	~Pending() {
		ReapList(list, true, true);
	}
};
thread_local Pending pending;
} // namespace

bool WritesBehind(const string &path) {
	auto slash = path.find_last_of('/');
	return StringUtil::StartsWith(slash == string::npos ? path : path.substr(slash + 1), "duckdb_temp_");
}

void WriteBehind(const std::shared_ptr<::xnvme::fs::File> &file, const void *buffer, idx_t nbytes, idx_t offset) {
	auto b = make_uniq<Behind>();
	b->file = file;
	auto bytes = static_cast<const uint8_t *>(buffer);
	b->bytes.assign(bytes, bytes + nbytes);
	b->offset = offset;
	{
		std::lock_guard<std::mutex> guard(registry_lock);
		// DuckDB reuses a block it freed while the write that held it may be in flight still, and the drive lands
		// two writes in either order; the older one is laid over reads until reaped, so it takes these bytes
		for (auto o : in_flight[file->Id()]) {
			idx_t from = MaxValue<idx_t>(offset, o->offset);
			idx_t to = MinValue<idx_t>(offset + nbytes, o->offset + o->bytes.size());
			if (from < to) {
				memcpy(o->bytes.data() + (from - o->offset), bytes + (from - offset), to - from);
				o->rewrite = true;
			}
		}
		in_flight[file->Id()].push_back(b.get());
	}
	Start(*b);
	pending.list.push_back(std::move(b));
}

void Reap(bool wait) {
	if (any_orphans) {
		std::lock_guard<std::mutex> guard(registry_lock);
		for (auto &b : orphans) {
			Again(*b);
			pending.list.push_back(std::move(b));
		}
		orphans.clear();
		any_orphans = false;
	}
	int failed = ReapList(pending.list, wait);
	if (failed) {
		throw IOException("xnvme fs: a write to a temporary file failed: %s", strerror(failed));
	}
}

Overlay::Overlay(const ::xnvme::fs::File &file, idx_t nbytes, idx_t offset) {
	std::lock_guard<std::mutex> guard(registry_lock);
	auto it = in_flight.find(file.Id());
	if (it == in_flight.end()) {
		return;
	}
	for (auto b : it->second) {
		idx_t from = MaxValue<idx_t>(offset, b->offset);
		idx_t to = MinValue<idx_t>(offset + nbytes, b->offset + b->bytes.size());
		if (from >= to) {
			continue;
		}
		auto src = b->bytes.data() + (from - b->offset);
		pieces.push_back({from - offset, vector<uint8_t>(src, src + (to - from))});
	}
}

void Overlay::Apply(void *buffer) const {
	for (auto &p : pieces) {
		memcpy(static_cast<uint8_t *>(buffer) + p.at, p.bytes.data(), p.bytes.size());
	}
}

bool InFlightPast(const ::xnvme::fs::File &file, idx_t size) {
	std::lock_guard<std::mutex> guard(registry_lock);
	auto it = in_flight.find(file.Id());
	if (it == in_flight.end()) {
		return false;
	}
	for (auto b : it->second) {
		if (b->offset + b->bytes.size() > size) {
			return true;
		}
	}
	return false;
}

} // namespace xnvme_fs
} // namespace duckdb
