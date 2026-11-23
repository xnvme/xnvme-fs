// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
// SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
//
// SPDX-License-Identifier: BSD-3-Clause

#include "session.hpp"

#include "error.hpp"
#include "trace.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace xnvme_fs {

File::File(std::shared_ptr<Session> session_p, std::string name_p, Opened opened)
    : session(std::move(session_p)), handle(opened.handle), name(std::move(name_p)), size(opened.size),
      extents(std::move(opened.extents)) {
}

File::~File() {
	// No other thread has the file once its last handle goes, so 'name' needs no lock here
	XNVME_FS_TRACE("close", name, size);
	try {
		std::lock_guard<std::mutex> guard(lock);
		WriteStage();
	} catch (...) { // NOLINT: a destructor cannot report; a handle's close calls Flush, which can
	}
	if (stage) {
		session->DmaMember().FreeDma(stage);
	}
	try {
		session->client.Close(handle, size);
	} catch (...) { // NOLINT: a destructor cannot report; the server reclaims the handle when the connection ends
	}
}

std::string File::Name() {
	std::lock_guard<std::mutex> guard(lock);
	return name;
}

uint64_t File::Size() {
	XNVME_FS_TRACE("size", Name());
	std::lock_guard<std::mutex> guard(lock);
	return size + Pending();
}

std::vector<File::Piece> File::Pieces(uint64_t nbytes, uint64_t offset) {
	std::vector<Piece> pieces;
	auto es = session->extent_size;
	while (nbytes > 0) {
		uint64_t index = offset / es;
		uint64_t within = offset % es;
		uint64_t n = std::min(nbytes, es - within);
		bool hole = index >= extents.size();
		pieces.push_back({hole ? 0 : extents[index], hole, within, n});
		offset += n;
		nbytes -= n;
	}
	return pieces;
}

static void RequestDone(Completion *c, void *arg) {
	auto r = static_cast<Request *>(arg);
	r->status = c->status;
	r->done = true;
}

void File::Submit(Request &r) {
	XNVME_FS_TRACE(r.op == Io::READ ? "submit_read" : "submit_write", Name(), r.offset, r.nbytes);
	Start(r);
}

void File::Start(Request &r) {
	r.done = false;
	r.status = 0;
	r.result = 0;
	r.members.clear();
	r.completion = Completion();
	r.completion.done = RequestDone;
	r.completion.arg = &r;
	// The submission counts as one part, so the request cannot complete before every piece is started
	r.completion.outstanding = 1;
	try {
		std::vector<Piece> pieces;
		{
			std::lock_guard<std::mutex> guard(lock);
			uint64_t bs = session->block_size;
			if (r.direct) {
				// The block holding the end lies on the drive whole unless appends to it wait in the stage
				uint64_t end = Pending() ? size / bs * bs : (size + bs - 1) / bs * bs;
				if (r.op != Io::READ || r.offset % bs || r.nbytes % bs || r.offset + r.nbytes > end) {
					Fail(EINVAL,
					     "xnvme fs: a direct %s of %llu bytes at %llu of '%s', of %llu bytes, is not of whole "
					     "blocks on the drives",
					     r.op == Io::READ ? "read" : "write", (unsigned long long)r.nbytes,
					     (unsigned long long)r.offset, name.c_str(), (unsigned long long)size);
				}
				pieces = Pieces(r.nbytes, r.offset);
				for (auto &p : pieces) {
					if (p.hole) {
						Fail(EINVAL, "xnvme fs: a direct read of '%s' at %llu meets a hole", name.c_str(),
						     (unsigned long long)r.offset);
					}
				}
				r.result = r.nbytes;
			} else if (r.op == Io::READ) {
				uint64_t end = size + Pending();
				r.result = r.offset >= end ? 0 : std::min(r.nbytes, end - r.offset);
				uint64_t stored = r.offset >= size ? 0 : std::min(r.result, size - r.offset);
				pieces = Pieces(stored, r.offset);
				if (r.result > stored) {
					memcpy(static_cast<uint8_t *>(r.buf) + stored, stage + (r.offset + stored - stage_at),
					       r.result - stored);
				}
			} else if (r.nbytes && r.offset == size + Pending() &&
			           (Pending() || size % bs || r.nbytes % bs || Held(r.nbytes))) {
				LoadStage();
				Stage(static_cast<const uint8_t *>(r.buf), r.nbytes);
				if (!Held(r.nbytes)) {
					WriteStage();
				}
				dirty = true;
				r.result = r.nbytes;
			} else if (r.offset + r.nbytes <= size / bs * bs) {
				// Below the block holding the end, so the stage does not hold these bytes; a truncate that grew the
				// file leased nothing for them
				LeaseTo(r.offset + r.nbytes);
				dirty = true;
				r.result = r.nbytes;
				pieces = Pieces(r.result, r.offset);
			} else {
				WriteStage();
				stage_at = NO_STAGE;
				LeaseTo(r.offset + r.nbytes);
				size = std::max(size, r.offset + r.nbytes);
				dirty = true;
				r.result = r.nbytes;
				pieces = Pieces(r.result, r.offset);
			}
		}
		auto bytes = static_cast<uint8_t *>(r.buf);
		uint64_t at = 0;
		for (auto &p : pieces) {
			if (p.hole) {
				memset(bytes + at, 0, p.nbytes);
			} else {
				auto &m = session->MemberOf(p.extent);
				if (std::find(r.members.begin(), r.members.end(), &m) == r.members.end()) {
					r.members.push_back(&m);
				}
				auto where = session->OffsetOf(p.extent, p.within);
				if (r.direct) {
					m.BeginReadDirect(bytes + at, p.nbytes, where, &r.completion);
				} else if (r.op == Io::READ) {
					m.BeginRead(bytes + at, p.nbytes, where, &r.completion);
				} else {
					m.BeginWrite(bytes + at, p.nbytes, where, &r.completion);
				}
			}
			at += p.nbytes;
		}
		// xNVMe's upcie queues hold a command until polled: the drives get them now, not once waited on
		for (auto m : r.members) {
			m->Poke();
		}
	} catch (Error &err) {
		// What was started writes into the caller's buffer, so it completes before the request does
		for (auto m : r.members) {
			try {
				m->Finish();
			} catch (Error &) { // NOLINT: the request reports the first failure, below
			}
		}
		if (!r.completion.status) {
			r.completion.status = err.Code() ? err.Code() : EIO;
			r.completion.why = err.what();
		}
	}
	if (--r.completion.outstanding == 0) {
		RequestDone(&r.completion, &r);
	}
}

void File::LeaseTo(uint64_t end) {
	uint64_t need = (end + session->extent_size - 1) / session->extent_size;
	while (extents.size() < need) {
		auto count = std::min<uint64_t>(need - extents.size(), MAX_LEASE);
		auto got = session->client.Lease(handle, uint32_t(count));
		extents.insert(extents.end(), got.begin(), got.end());
	}
}

bool File::Held(uint64_t nbytes) const {
	return session->buffer_appends && nbytes < session->DmaMember().CommandBytes();
}

uint64_t File::Pending() const {
	return stage_at == NO_STAGE ? 0 : stage_at + stage_fill - size;
}

void File::LoadStage() {
	if (stage_at != NO_STAGE) {
		return;
	}
	uint64_t bs = session->block_size;
	if (!stage) {
		auto &m = session->DmaMember();
		stage_nbytes = std::max<uint64_t>(m.CommandBytes() / bs * bs, bs);
		stage = static_cast<uint8_t *>(m.AllocDma(stage_nbytes));
	}
	uint64_t start = size / bs * bs;
	uint64_t head = size - start;
	memset(stage, 0, stage_nbytes);
	// Past the end of the file the drive holds zeros: extents are cleared when leased, and Truncate clears the rest
	// of a block it cuts. So only a block holding bytes of the file is read, and not one a truncate grew the file to.
	if (head && start / session->extent_size < extents.size()) {
		auto e = extents[start / session->extent_size];
		auto &m = session->MemberOf(e);
		Completion c;
		c.outstanding = 1;
		m.BeginReadDirect(stage, bs, session->OffsetOf(e, start % session->extent_size), &c);
		c.outstanding--;
		struct Tail {
			Member *m;
			Completion *c;
		} tail {&m, &c};
		PacedWait(
		    WaitClass(bs),
		    [](void *arg) {
			    auto t = static_cast<Tail *>(arg);
			    t->m->Poke();
			    return !t->c->outstanding;
		    },
		    &tail);
		if (c.status) {
			Fail(c.status, "xnvme fs: reading the last block of '%s' failed: %s", name.c_str(), c.why.c_str());
		}
		memset(stage + head, 0, bs - head);
	}
	stage_at = start;
	stage_fill = head;
}

void File::Stage(const uint8_t *bytes, uint64_t nbytes) {
	while (nbytes) {
		uint64_t take = std::min(nbytes, stage_nbytes - stage_fill);
		memcpy(stage + stage_fill, bytes, take);
		stage_fill += take;
		bytes += take;
		nbytes -= take;
		if (stage_fill == stage_nbytes) {
			WriteStage();
		}
	}
}

void File::WriteStage() {
	if (!Pending()) {
		return;
	}
	uint64_t bs = session->block_size;
	uint64_t padded = (stage_fill + bs - 1) / bs * bs;
	LeaseTo(stage_at + padded);
	Completion c;
	c.outstanding = 1;
	std::vector<Member *> used;
	uint64_t at = 0;
	try {
		for (auto &p : Pieces(padded, stage_at)) {
			auto &m = session->MemberOf(p.extent);
			if (std::find(used.begin(), used.end(), &m) == used.end()) {
				used.push_back(&m);
			}
			m.BeginWriteDirect(stage + at, p.nbytes, session->OffsetOf(p.extent, p.within), &c);
			at += p.nbytes;
		}
	} catch (Error &) {
		// Commands already started report to 'c', so they complete before it goes
		for (auto m : used) {
			try {
				m->Finish();
			} catch (Error &) { // NOLINT: the first failure is the one passed on
			}
		}
		throw;
	}
	c.outstanding--;
	struct Landing {
		std::vector<Member *> *used;
		Completion *c;
	} landing {&used, &c};
	PacedWait(
	    WaitClass(padded),
	    [](void *arg) {
		    auto l = static_cast<Landing *>(arg);
		    for (auto m : *l->used) {
			    m->Poke();
		    }
		    return !l->c->outstanding;
	    },
	    &landing);
	if (c.status) {
		Fail(c.status, "xnvme fs: writing %llu bytes at %llu to '%s' failed: %s", (unsigned long long)padded,
		     (unsigned long long)stage_at, name.c_str(), c.why.c_str());
	}
	size = stage_at + stage_fill;
	// The block holding the new end stays, moved to the front: less than a block, once per write of the stage
	uint64_t keep = size / bs * bs - stage_at;
	if (keep) {
		uint64_t head = stage_fill - keep;
		memmove(stage, stage + keep, head);
		memset(stage + head, 0, stage_fill - head);
		stage_at += keep;
		stage_fill = head;
	}
}

void File::Flush() {
	std::lock_guard<std::mutex> guard(lock);
	WriteStage();
}

bool Poll(Request &r) {
	for (auto m : r.members) {
		if (r.done) {
			break;
		}
		m->Poke();
	}
	return r.done;
}

void Wait(Request &r) {
	PacedWait(
	    WaitClass(r.nbytes), [](void *arg) { return Poll(*static_cast<Request *>(arg)); }, &r);
}

uint64_t File::Read(void *buffer, uint64_t nbytes, uint64_t offset) {
	Request r;
	r.op = Io::READ;
	r.buf = buffer;
	r.nbytes = nbytes;
	r.offset = offset;
	XNVME_FS_TRACE("read", Name(), offset, nbytes);
	Start(r);
	Wait(r);
	if (r.status) {
		Fail(r.status, "xnvme fs: read of %llu bytes at %llu from '%s' failed: %s", (unsigned long long)nbytes,
		     (unsigned long long)offset, Name().c_str(), r.completion.why.c_str());
	}
	return r.result;
}

void File::Write(const void *buffer, uint64_t nbytes, uint64_t offset) {
	Request r;
	r.op = Io::WRITE;
	r.buf = const_cast<void *>(buffer);
	r.nbytes = nbytes;
	r.offset = offset;
	XNVME_FS_TRACE("write", Name(), offset, nbytes);
	Start(r);
	Wait(r);
	if (r.status) {
		Fail(r.status, "xnvme fs: write of %llu bytes at %llu to '%s' failed: %s", (unsigned long long)nbytes,
		     (unsigned long long)offset, Name().c_str(), r.completion.why.c_str());
	}
}

void File::Sync() {
	XNVME_FS_TRACE("sync", Name());
	std::lock_guard<std::mutex> guard(lock);
	WriteStage();
	if (!dirty) {
		return;
	}
	// The data must be on the drive before a map that names it is.
	session->SyncMembers();
	session->client.Commit(handle, size);
	dirty = false;
}

void File::Truncate(uint64_t new_size) {
	XNVME_FS_TRACE("truncate", Name(), new_size);
	std::lock_guard<std::mutex> guard(lock);
	WriteStage();
	// The extent holding the new end keeps what lay past it; cleared, a later write beyond leaves zeros in between
	auto es = session->extent_size;
	auto index = new_size / es;
	if (new_size < size && new_size % es && index < extents.size()) {
		std::vector<uint8_t> zeros(std::min(size, (index + 1) * es) - new_size, 0);
		session->MemberOf(extents[index])
		    .Write(zeros.data(), zeros.size(), session->OffsetOf(extents[index], new_size % es));
	}
	session->client.Truncate(handle, new_size);
	stage_at = NO_STAGE;
	auto keep = (new_size + es - 1) / es;
	if (extents.size() > keep) {
		extents.resize(keep);
	}
	size = new_size;
	dirty = true;
}

Session::Session(const std::string &pool_p, const std::string &socket_path_p, const Access &access)
    : pool(pool_p), socket_path(socket_path_p), client(socket_path_p, pool_p) {
	extent_size = client.Pool().extent_size;
	for (auto &m : client.Pool().members) {
		members.push_back(std::unique_ptr<Member>(new Member(m, access)));
		block_size = std::max(block_size, members.back()->BlockSize());
	}
	auto buffer = getenv("XNVME_FS_APPEND_BUFFER");
	buffer_appends = buffer && *buffer && strcmp(buffer, "0") != 0;
	auto stats = getenv("XNVME_FS_STATS");
	if (stats && *stats) {
		sampler = std::thread(&Session::Sample, this, std::string(stats));
	}
}

Session::~Session() {
	if (sampler.joinable()) {
		stop_sampling = true;
		sampler.join();
	}
}

//! Samples each member's commands in flight every millisecond, as the kernel's io_ticks and weighted time count the
//! drive's, and appends a line per member to 'path' every second and at the end: the time, the process, the member,
//! then since the start the samples, those with a command and with a read in flight, the commands and the reads in
//! flight summed over the samples, and the reads, writes and their bytes completed. At the end, per member, a line
//! for each read latency class follows: the time, the process, the member, 'latency', the class, then its buckets.
void Session::Sample(const std::string &path) {
	struct Sum {
		uint64_t samples = 0, busy = 0, read_busy = 0, depth = 0, read_depth = 0;
	};
	std::vector<Sum> sums(members.size());
	auto next = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	for (bool last = false; !last;) {
		last = stop_sampling;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		for (size_t i = 0; i < members.size(); i++) {
			auto all = std::max<int64_t>(members[i]->stats.in_flight, 0);
			auto reads = std::max<int64_t>(members[i]->stats.reads_in_flight, 0);
			auto &sum = sums[i];
			sum.samples++;
			sum.busy += all > 0;
			sum.read_busy += reads > 0;
			sum.depth += uint64_t(all);
			sum.read_depth += uint64_t(reads);
		}
		if (!last && std::chrono::steady_clock::now() < next) {
			continue;
		}
		next += std::chrono::seconds(1);
		auto f = fopen(path.c_str(), "a");
		if (!f) {
			continue;
		}
		double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
		for (size_t i = 0; i < members.size(); i++) {
			auto &s = members[i]->stats;
			auto &sum = sums[i];
			fprintf(f, "%.3f %d %zu %llu %llu %llu %llu %llu %llu %llu %llu %llu\n", now, int(getpid()), i,
			        (unsigned long long)sum.samples, (unsigned long long)sum.busy, (unsigned long long)sum.read_busy,
			        (unsigned long long)sum.depth, (unsigned long long)sum.read_depth, (unsigned long long)s.reads,
			        (unsigned long long)s.writes, (unsigned long long)s.read_bytes, (unsigned long long)s.write_bytes);
			for (int c = 0; last && c < Member::Stats::LATENCY_CLASSES; c++) {
				fprintf(f, "%.3f %d %zu latency %d", now, int(getpid()), i, c);
				for (auto &n : s.read_latency[c]) {
					fprintf(f, " %llu", (unsigned long long)n.load());
				}
				fprintf(f, "\n");
			}
		}
		fclose(f);
	}
}

std::shared_ptr<File> Session::Open(const std::string &name, uint32_t flags) {
	XNVME_FS_TRACE("open", name, flags);
	std::lock_guard<std::mutex> guard(files_lock);
	auto it = files.find(name);
	if (it != files.end()) {
		auto file = it->second.lock();
		if (file) {
			if ((flags & OPEN_CREATE) && (flags & OPEN_EXCLUSIVE)) {
				Fail(EEXIST, "xnvme fs: '%s' exists", name.c_str());
			}
			if (flags & OPEN_TRUNCATE) {
				file->Truncate(0);
			}
			return file;
		}
	}
	auto file = std::make_shared<File>(shared_from_this(), name, client.Open(name, flags));
	files[name] = file;
	return file;
}

std::shared_ptr<File> Session::Find(const std::string &name) {
	std::lock_guard<std::mutex> guard(files_lock);
	auto it = files.find(name);
	return it == files.end() ? nullptr : it->second.lock();
}

void Session::Forget(const std::string &name) {
	std::lock_guard<std::mutex> guard(files_lock);
	files.erase(name);
}

void Session::ForgetUnder(const std::string &directory) {
	auto prefix = directory + "/";
	std::lock_guard<std::mutex> guard(files_lock);
	for (auto it = files.begin(); it != files.end();) {
		it = it->first.compare(0, prefix.size(), prefix) == 0 ? files.erase(it) : std::next(it);
	}
}

void Session::Renamed(const std::string &from, const std::string &to) {
	// A rename onto itself changes nothing, and erasing 'to' would erase the file renamed
	if (from == to) {
		return;
	}
	std::lock_guard<std::mutex> guard(files_lock);
	files.erase(to);
	auto it = files.find(from);
	if (it != files.end()) {
		auto file = it->second;
		files.erase(it);
		files[to] = file;
		if (auto f = file.lock()) {
			std::lock_guard<std::mutex> named(f->lock);
			f->name = to;
		}
	}
}

void Session::SyncMembers() {
	for (auto &m : members) {
		m->BeginFlush();
	}
	for (auto &m : members) {
		m->Finish();
	}
}

} // namespace xnvme_fs
