// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
// SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
//
// SPDX-License-Identifier: BSD-3-Clause

#include <libxnvme_fs.h>

#include "error.hpp"
#include "session.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

using namespace xnvme_fs;

struct xnvme_fs_session {
	std::shared_ptr<Session> session;
};

struct xnvme_fs_file {
	std::shared_ptr<File> file;
};

struct xnvme_fs_req {
	Request request;
};

//! An array, not a std::string: a call made from the destructor of another thread_local, at thread exit, would use
//! a string after its own destructor ran
static thread_local char errmsg[512];

static void SetErrmsg(const char *why) {
	snprintf(errmsg, sizeof(errmsg), "%s", why);
}

template <class F>
static auto Guard(F f) -> decltype(f()) {
	try {
		return f();
	} catch (Error &err) {
		SetErrmsg(err.what());
		return -(err.Code() ? err.Code() : EIO);
	} catch (std::bad_alloc &) {
		SetErrmsg("xnvme fs: out of memory");
		return -ENOMEM;
	} catch (std::exception &err) {
		SetErrmsg(err.what());
		return -EIO;
	} catch (...) {
		SetErrmsg("xnvme fs: unknown error");
		return -EIO;
	}
}

//! Fails the call with EINVAL where the caller passed NULL for 'what'
static_assert(XNVME_FS_NAME_MAX == xnvme_fs::MAX_FILE_NAME, "a name in the pool fits an entry");
static_assert(sizeof(struct xnvme_fs_entry) == 272, "struct xnvme_fs_entry has no padding");

static void Given(const void *arg, const char *call, const char *what) {
	if (!arg) {
		Fail(EINVAL, "xnvme fs: %s: '%s' is NULL", call, what);
	}
}

extern "C" {

const char *xnvme_fs_errmsg(void) {
	return errmsg;
}

int xnvme_fs_connect(const char *pool, const char *socket_dir, const char *backend, uint32_t homi_id,
                     struct xnvme_fs_session **s) {
	return Guard([&]() -> int {
		Given(s, "xnvme_fs_connect", "s");
		*s = nullptr;
		Given(pool, "xnvme_fs_connect", "pool");
		Given(socket_dir, "xnvme_fs_connect", "socket_dir");
		Access access;
		if (backend && *backend) {
			access.backend = backend;
		}
		access.homi_id = homi_id;
		auto path = std::string(socket_dir) + "/" + pool + ".sock";
		*s = new xnvme_fs_session {std::make_shared<Session>(pool, path, access)};
		return 0;
	});
}

void xnvme_fs_disconnect(struct xnvme_fs_session *s) {
	delete s;
}

int xnvme_fs_open(struct xnvme_fs_session *s, const char *name, uint32_t flags, struct xnvme_fs_file **f) {
	return Guard([&]() -> int {
		Given(f, "xnvme_fs_open", "f");
		*f = nullptr;
		Given(name, "xnvme_fs_open", "name");
		uint32_t open_flags = 0;
		open_flags |= (flags & XNVME_FS_O_CREATE) ? uint32_t(OPEN_CREATE) : 0u;
		open_flags |= (flags & XNVME_FS_O_TRUNC) ? uint32_t(OPEN_TRUNCATE) : 0u;
		open_flags |= (flags & XNVME_FS_O_EXCL) ? uint32_t(OPEN_EXCLUSIVE) : 0u;
		*f = new xnvme_fs_file {s->session->Open(name, open_flags)};
		return 0;
	});
}

int xnvme_fs_close(struct xnvme_fs_file *f) {
	int rc = Guard([&]() -> int {
		f->file->Flush();
		return 0;
	});
	delete f;
	return rc;
}

int64_t xnvme_fs_size(struct xnvme_fs_file *f) {
	return Guard([&]() -> int64_t { return int64_t(f->file->Size()); });
}

ssize_t xnvme_fs_pread(struct xnvme_fs_file *f, void *buf, uint64_t nbytes, uint64_t offset) {
	return Guard([&]() -> ssize_t { return ssize_t(f->file->Read(buf, nbytes, offset)); });
}

ssize_t xnvme_fs_pwrite(struct xnvme_fs_file *f, const void *buf, uint64_t nbytes, uint64_t offset) {
	return Guard([&]() -> ssize_t {
		f->file->Write(buf, nbytes, offset);
		return ssize_t(nbytes);
	});
}

int xnvme_fs_fsync(struct xnvme_fs_file *f) {
	return Guard([&]() -> int {
		f->file->Sync();
		return 0;
	});
}

int xnvme_fs_ftruncate(struct xnvme_fs_file *f, uint64_t size) {
	return Guard([&]() -> int {
		f->file->Truncate(size);
		return 0;
	});
}

int xnvme_fs_stat(struct xnvme_fs_session *s, const char *name, struct xnvme_fs_stat *st) {
	return Guard([&]() -> int {
		Given(name, "xnvme_fs_stat", "name");
		Given(st, "xnvme_fs_stat", "st");
		auto found = s->session->client.StatName(name);
		st->kind = found.kind;
		st->size = found.size;
		// The server knows a size as of the last commit; a file open here may be longer
		if (auto file = s->session->Find(name)) {
			st->size = file->Size();
		}
		return 0;
	});
}

int xnvme_fs_list(struct xnvme_fs_session *s, const char *directory, uint64_t offset,
                  struct xnvme_fs_entry *entries, uint32_t max, uint32_t *count, uint64_t *remaining,
                  uint64_t *version) {
	return Guard([&]() -> int {
		Given(directory, "xnvme_fs_list", "directory");
		Given(count, "xnvme_fs_list", "count");
		Given(remaining, "xnvme_fs_list", "remaining");
		Given(version, "xnvme_fs_list", "version");
		*count = 0;
		if (max) {
			Given(entries, "xnvme_fs_list", "entries");
		}
		auto listing = s->session->client.List(directory, offset, max);
		if (*version && *version != listing.version) {
			Fail(ESTALE, "xnvme fs: the names in the pool changed while listing '%s'", directory);
		}
		std::string prefix = *directory ? std::string(directory) + "/" : std::string();
		uint32_t n = std::min<uint32_t>(max, uint32_t(listing.entries.size()));
		for (uint32_t i = 0; i < n; i++) {
			auto &e = listing.entries[i];
			entries[i] = {};
			entries[i].size = e.size;
			entries[i].kind = uint8_t(e.kind);
			if (e.kind == EntryKind::FILE) {
				if (auto file = s->session->Find(prefix + e.name)) {
					entries[i].size = file->Size();
				}
			}
			std::memcpy(entries[i].name, e.name.c_str(), std::min(e.name.size(), size_t(XNVME_FS_NAME_MAX)));
		}
		*count = n;
		*remaining = listing.remaining + (listing.entries.size() - n);
		*version = listing.version;
		return 0;
	});
}

int xnvme_fs_mkdir(struct xnvme_fs_session *s, const char *name) {
	return Guard([&]() -> int {
		Given(name, "xnvme_fs_mkdir", "name");
		s->session->client.Mkdir(name);
		return 0;
	});
}

int xnvme_fs_rmdir(struct xnvme_fs_session *s, const char *name) {
	return Guard([&]() -> int {
		Given(name, "xnvme_fs_rmdir", "name");
		s->session->client.Rmdir(name);
		s->session->ForgetUnder(name);
		return 0;
	});
}

int xnvme_fs_remove(struct xnvme_fs_session *s, const char *name) {
	return Guard([&]() -> int {
		Given(name, "xnvme_fs_remove", "name");
		s->session->client.Remove(name);
		s->session->Forget(name);
		return 0;
	});
}

int xnvme_fs_rename(struct xnvme_fs_session *s, const char *from, const char *to) {
	return Guard([&]() -> int {
		Given(from, "xnvme_fs_rename", "from");
		Given(to, "xnvme_fs_rename", "to");
		s->session->client.Rename(from, to);
		s->session->Renamed(from, to);
		return 0;
	});
}

int xnvme_fs_info(struct xnvme_fs_session *s, struct xnvme_fs_info *info) {
	return Guard([&]() -> int {
		Given(info, "xnvme_fs_info", "info");
		auto stats = s->session->client.Info();
		info->layout_version = stats.layout_version;
		info->nmembers = uint32_t(stats.members.size());
		info->extent_size = stats.extent_size;
		info->generation = stats.generation;
		info->files = stats.files;
		return 0;
	});
}

int xnvme_fs_members(struct xnvme_fs_session *s, uint64_t offset, struct xnvme_fs_member *members,
                     uint32_t max, uint32_t *count, uint64_t *remaining) {
	return Guard([&]() -> int {
		Given(count, "xnvme_fs_members", "count");
		Given(remaining, "xnvme_fs_members", "remaining");
		*count = 0;
		if (max) {
			Given(members, "xnvme_fs_members", "members");
		}
		// A pool has few members, so the server sends them all and the page is cut here
		auto stats = s->session->client.Info();
		uint64_t total = stats.members.size();
		uint64_t first = std::min<uint64_t>(offset, total);
		uint32_t n = uint32_t(std::min<uint64_t>(max, total - first));
		for (uint32_t i = 0; i < n; i++) {
			auto &m = stats.members[first + i];
			if (m.path.size() > XNVME_FS_NAME_MAX) {
				Fail(ENAMETOOLONG, "xnvme fs: the path of member %llu is longer than %d bytes",
				     (unsigned long long)(first + i), XNVME_FS_NAME_MAX);
			}
			members[i] = {};
			members[i].size = m.size;
			members[i].extents = m.extents;
			members[i].free_extents =
			    first + i < stats.free_extents.size() ? stats.free_extents[first + i] : 0;
			std::memcpy(members[i].path, m.path.c_str(), m.path.size());
		}
		*count = n;
		*remaining = total - first - n;
		return 0;
	});
}

int xnvme_fs_req_new(struct xnvme_fs_req **r) {
	return Guard([&]() -> int {
		Given(r, "xnvme_fs_req_new", "r");
		*r = nullptr;
		*r = new xnvme_fs_req();
		return 0;
	});
}

void xnvme_fs_req_free(struct xnvme_fs_req *r) {
	delete r;
}

int xnvme_fs_submit(struct xnvme_fs_file *f, struct xnvme_fs_req *r, int write, void *buf, uint64_t nbytes,
                   uint64_t offset) {
	return Guard([&]() -> int {
		auto &q = r->request;
		q.op = write ? Io::WRITE : Io::READ;
		q.direct = false;
		q.buf = buf;
		q.nbytes = nbytes;
		q.offset = offset;
		f->file->Submit(q);
		return 0;
	});
}

int xnvme_fs_submit_direct(struct xnvme_fs_file *f, struct xnvme_fs_req *r, void *dma, uint64_t nbytes, uint64_t offset) {
	return Guard([&]() -> int {
		auto &q = r->request;
		q.op = Io::READ;
		q.direct = true;
		q.buf = dma;
		q.nbytes = nbytes;
		q.offset = offset;
		f->file->Submit(q);
		return 0;
	});
}

uint32_t xnvme_fs_block_size(struct xnvme_fs_session *s) {
	return s->session->block_size;
}

void *xnvme_fs_dma_alloc(struct xnvme_fs_session *s, uint64_t nbytes) {
	void *p = nullptr;
	int err = Guard([&]() -> int {
		p = s->session->DmaMember().AllocDma(nbytes);
		return 0;
	});
	if (!p) {
		errno = err ? -err : ENOMEM;
	}
	return p;
}

void xnvme_fs_dma_free(struct xnvme_fs_session *s, void *p) {
	s->session->DmaMember().FreeDma(p);
}

int xnvme_fs_register_dmabuf(struct xnvme_fs_session *s, int fd, void *vaddr) {
	return Guard([&]() -> int {
		s->session->DmaMember().RegisterDmabuf(fd, vaddr);
		return 0;
	});
}

int xnvme_fs_unregister_dmabuf(struct xnvme_fs_session *s, void *vaddr) {
	return Guard([&]() -> int {
		s->session->DmaMember().UnregisterDmabuf(vaddr);
		return 0;
	});
}

int xnvme_fs_poll(struct xnvme_fs_req *r, int *done) {
	return Guard([&]() -> int {
		Given(done, "xnvme_fs_poll", "done");
		*done = 0;
		*done = Poll(r->request) ? 1 : 0;
		return 0;
	});
}

ssize_t xnvme_fs_wait(struct xnvme_fs_req *r) {
	return Guard([&]() -> ssize_t {
		Wait(r->request);
		if (r->request.status) {
			SetErrmsg(r->request.completion.why.c_str());
			return -ssize_t(r->request.status);
		}
		return ssize_t(r->request.result);
	});
}

} // extern "C"
