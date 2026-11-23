// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
// SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
//
// SPDX-License-Identifier: BSD-3-Clause

#include "protocol.hpp"
#include "trace.hpp"

#include "wire.hpp"

#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace xnvme_fs {

static void WriteAll(int fd, const uint8_t *p, size_t n) {
	while (n > 0) {
		ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
		if (w < 0 && errno == EINTR) {
			continue;
		}
		if (w <= 0) {
			Fail(w < 0 ? errno : EPIPE, "xnvme fs: metadata socket write failed: %s", strerror(w < 0 ? errno : EPIPE));
		}
		p += w;
		n -= size_t(w);
	}
}

//! False on end of stream before the first byte; throws on one in the middle.
static bool ReadAll(int fd, uint8_t *p, size_t n) {
	size_t got = 0;
	while (got < n) {
		ssize_t r = recv(fd, p + got, n - got, 0);
		if (r < 0 && errno == EINTR) {
			continue;
		}
		if (r < 0) {
			Fail(errno, "xnvme fs: metadata socket read failed: %s", strerror(errno));
		}
		if (r == 0) {
			if (got == 0) {
				return false;
			}
			Fail(EPROTO, "xnvme fs: metadata socket closed inside a frame");
		}
		got += size_t(r);
	}
	return true;
}

void SendFrame(int fd, const std::vector<uint8_t> &body) {
	Writer w;
	w.U32(uint32_t(body.size()));
	w.buf.insert(w.buf.end(), body.begin(), body.end());
	WriteAll(fd, w.buf.data(), w.buf.size());
}

bool RecvFrame(int fd, std::vector<uint8_t> &body) {
	uint8_t len[4];
	if (!ReadAll(fd, len, sizeof(len))) {
		return false;
	}
	uint32_t n = Reader(len, 4).U32();
	if (n > MAX_FRAME) {
		Fail(EPROTO, "xnvme fs: frame of %u bytes exceeds the limit", n);
	}
	body.resize(n);
	if (n > 0 && !ReadAll(fd, body.data(), n)) {
		Fail(EPROTO, "xnvme fs: metadata socket closed inside a frame");
	}
	return true;
}

static Writer Request(Op op) {
	Writer w;
	w.U8(uint8_t(op));
	return w;
}

Client::Client(const std::string &socket_path, const std::string &pool) {
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	if (socket_path.size() >= sizeof(addr.sun_path)) {
		Fail(ENAMETOOLONG, "xnvme fs: socket path '%s' is too long", socket_path.c_str());
	}
	memcpy(addr.sun_path, socket_path.data(), socket_path.size());
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		Fail(errno, "xnvme fs: cannot create a socket: %s", strerror(errno));
	}
	if (connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
		int saved = errno;
		close(fd);
		fd = -1;
		Fail(saved, "xnvme fs: cannot reach the pool's server for pool '%s' at '%s': %s", pool.c_str(), socket_path.c_str(),
		     strerror(saved));
	}
	auto w = Request(Op::HELLO);
	w.U32(PROTOCOL_VERSION);
	w.Str(pool);
	auto body = Call(w.buf);
	Reader r(body.data(), body.size());
	hello.extent_size = r.U64();
	hello.data_offset = r.U64();
	auto n = r.U32();
	for (uint32_t i = 0; i < n; i++) {
		hello.members.push_back(r.Str());
	}
}

Client::~Client() {
	if (fd >= 0) {
		close(fd);
	}
}

std::vector<uint8_t> Client::Call(const std::vector<uint8_t> &request) {
	std::lock_guard<std::mutex> guard(lock);
	if (fd < 0) {
		Fail(ENOTCONN, "xnvme fs: the connection to the pool's server has failed");
	}
	std::vector<uint8_t> body;
	try {
		SendFrame(fd, request);
		if (!RecvFrame(fd, body)) {
			Fail(ECONNRESET, "xnvme fs: the server closed the connection");
		}
	} catch (Error &) {
		close(fd);
		fd = -1;
		throw;
	}
	Reader r(body.data(), body.size());
	int32_t status = r.I32();
	if (status != 0) {
		throw Error(status, r.Str());
	}
	return std::vector<uint8_t>(body.begin() + 4, body.end());
}

Opened Client::Open(const std::string &name, uint32_t flags) {
	auto w = Request(Op::OPEN);
	w.Str(name);
	w.U32(flags);
	auto body = Call(w.buf);
	Reader r(body.data(), body.size());
	Opened o;
	o.handle = r.U64();
	o.size = r.U64();
	o.extents = r.U64s();
	return o;
}

void Client::Close(uint64_t handle, uint64_t size) {
	auto w = Request(Op::CLOSE);
	w.U64(handle);
	w.U64(size);
	Call(w.buf);
}

std::vector<ExtentAddr> Client::Lease(uint64_t handle, uint32_t count) {
	XNVME_FS_TRACE("lease", "", handle, count);
	auto w = Request(Op::LEASE);
	w.U64(handle);
	w.U32(count);
	auto body = Call(w.buf);
	Reader r(body.data(), body.size());
	return r.U64s();
}

void Client::Commit(uint64_t handle, uint64_t size) {
	XNVME_FS_TRACE("commit", "", handle, size);
	auto w = Request(Op::COMMIT);
	w.U64(handle);
	w.U64(size);
	Call(w.buf);
}

void Client::Truncate(uint64_t handle, uint64_t size) {
	auto w = Request(Op::TRUNCATE);
	w.U64(handle);
	w.U64(size);
	Call(w.buf);
}

Stat Client::StatName(const std::string &name) {
	XNVME_FS_TRACE("stat", name);
	auto w = Request(Op::STAT);
	w.Str(name);
	auto body = Call(w.buf);
	Reader r(body.data(), body.size());
	Stat s;
	s.kind = r.U8();
	s.size = r.U64();
	return s;
}

Listing Client::List(const std::string &directory, uint64_t offset, uint32_t max) {
	XNVME_FS_TRACE("list", directory, offset, max);
	auto w = Request(Op::LIST);
	w.Str(directory);
	w.U64(offset);
	w.U32(max);
	auto body = Call(w.buf);
	Reader r(body.data(), body.size());
	Listing l;
	l.version = r.U64();
	l.remaining = r.U64();
	l.entries.resize(r.U32());
	for (auto &e : l.entries) {
		e.name = r.Str();
		e.kind = EntryKind(r.U8());
		e.size = r.U64();
	}
	return l;
}

void Client::Mkdir(const std::string &name) {
	XNVME_FS_TRACE("mkdir", name);
	auto w = Request(Op::MKDIR);
	w.Str(name);
	Call(w.buf);
}

void Client::Rmdir(const std::string &name) {
	XNVME_FS_TRACE("rmdir", name);
	auto w = Request(Op::RMDIR);
	w.Str(name);
	Call(w.buf);
}

void Client::Remove(const std::string &name) {
	XNVME_FS_TRACE("remove", name);
	auto w = Request(Op::REMOVE);
	w.Str(name);
	Call(w.buf);
}

void Client::Rename(const std::string &from, const std::string &to) {
	XNVME_FS_TRACE("rename", from + ">" + to);
	auto w = Request(Op::RENAME);
	w.Str(from);
	w.Str(to);
	Call(w.buf);
}

PoolStats Client::Info() {
	auto body = Call(Request(Op::INFO).buf);
	Reader r(body.data(), body.size());
	PoolStats s;
	s.name = r.Str();
	s.layout_version = r.U32();
	s.extent_size = r.U64();
	s.generation = r.U64();
	s.files = r.U64();
	auto n = r.U32();
	for (uint32_t i = 0; i < n; i++) {
		MemberInfo m;
		m.path = r.Str();
		m.size = r.U64();
		m.extents = r.U64();
		s.members.push_back(m);
		s.free_extents.push_back(r.U64());
	}
	return s;
}

Index Client::MakeIndex() {
	auto body = Call(Request(Op::INDEX).buf);
	Reader r(body.data(), body.size());
	Index ix;
	ix.lease = r.U64();
	ix.version = r.U64();
	ix.extent_size = r.U64();
	ix.data_offset = r.U64();
	ix.members.resize(r.U32());
	for (auto &m : ix.members) {
		m.path = r.Str();
		m.stripe = r.U64();
		m.places.resize(r.U32());
		for (auto &p : m.places) {
			p.drive = r.Str();
			p.base = r.U64();
		}
	}
	ix.directories.resize(r.U32());
	for (auto &d : ix.directories) {
		d = r.Str();
	}
	ix.files.resize(r.U32());
	for (auto &f : ix.files) {
		f.name = r.Str();
		f.size = r.U64();
		f.extents = r.U64s();
	}
	return ix;
}

uint64_t Client::Watch(uint64_t lease) {
	auto w = Request(Op::WATCH);
	w.U64(lease);
	auto body = Call(w.buf);
	Reader r(body.data(), body.size());
	return r.U64();
}

void Client::Release(uint64_t lease) {
	auto w = Request(Op::RELEASE);
	w.U64(lease);
	Call(w.buf);
}

} // namespace xnvme_fs
