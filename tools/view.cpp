// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
// SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
//
// SPDX-License-Identifier: BSD-3-Clause

// The read-only FUSE view of a pool: the namespace as the pool's server holds it, and file contents read through
// the server's own queues, beside the engines' I/O on the same controllers. Spoken over /dev/fuse with ufuse.

#include "server.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <dirent.h>
#include <sys/mount.h>
#include <ufuse.h>

namespace xnvme_fs {

struct Server::ViewState {
	ufuse_session session;
	std::string mountpoint;
	std::vector<uint8_t> buf;
	// Inodes are handed out by path on first sight and kept; a name removed and made again gets its old one.
	std::map<std::string, uint64_t> ino_of;
	std::vector<std::string> path_of;

	uint64_t Ino(const std::string &path) {
		auto it = ino_of.find(path);
		if (it != ino_of.end()) {
			return it->second;
		}
		path_of.push_back(path);
		ino_of[path] = path_of.size();
		return path_of.size();
	}
	const std::string *Path(uint64_t ino) const {
		return ino == 0 || ino > path_of.size() ? nullptr : &path_of[ino - 1];
	}
};

static std::string Join(const std::string &dir, const std::string &name) {
	return dir.empty() ? name : dir + "/" + name;
}

static std::string Parent(const std::string &path) {
	auto slash = path.rfind('/');
	return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

void Server::ViewDeleter::operator()(ViewState *v) const {
	delete v;
}

Server::~Server() {
	CloseView();
}

void Server::OpenView(const std::string &mountpoint) {
	std::unique_ptr<ViewState, ViewDeleter> v(new ViewState());
	ufuse_session_init(&v->session);
	v->mountpoint = mountpoint;
	v->buf.resize(UFUSE_BUF_NBYTES);
	v->Ino(""); // the root is inode 1, FUSE_ROOT_ID
	ufuse_unmount(&v->session, mountpoint.c_str());
	int err = ufuse_mount(&v->session, mountpoint.c_str(), sb.pool_name.c_str(), "xnvme-fs", MS_RDONLY,
	                      "allow_other,default_permissions");
	if (err) {
		Fail(-err, "xnvme fs: cannot mount the view at '%s': %s", mountpoint.c_str(), strerror(-err));
	}
	view = std::move(v);
}

void Server::CloseView() {
	if (!view) {
		return;
	}
	ufuse_unmount(&view->session, view->mountpoint.c_str());
	view.reset();
}

int Server::ViewFd() const {
	return view ? view->session.fd : -1;
}

// The view's answers, one per request it serves; ufuse_dispatch decodes the rest away: mutating requests and
// opens for writing get EROFS, and requests without an answer here ENOSYS
struct Server::ViewOps {
	static Server &Of(const ufuse_req *req) {
		return *static_cast<Server *>(req->ctx);
	}
	//! The path of inode 'ino', copied, since handing out an inode may grow the table it lives in; false if unknown
	static bool PathOf(Server &srv, uint64_t ino, std::string &path) {
		auto known = srv.view->Path(ino);
		if (known) {
			path = *known;
		}
		return known != nullptr;
	}
	// A file's size is what the server last heard from its client: committed, truncated or closed
	static bool Attr(Server &srv, const std::string &p, uint64_t node, struct fuse_attr *a) {
		auto f = srv.files.find(p);
		if (f != srv.files.end()) {
			ufuse_attr_fill(a, node, S_IFREG | 0444, 1, f->second->size);
			return true;
		}
		if (srv.IsDirectory(p)) {
			ufuse_attr_fill(a, node, S_IFDIR | 0555, 2, 0);
			return true;
		}
		return false;
	}

	//! Seconds the kernel may keep attributes: long for a file, whose size changes the server notifies; short for a
	//! directory, whose entries come and go with only the entry timeout to show it
	static uint64_t AttrValid(const struct fuse_attr &a) {
		return S_ISREG(a.mode) ? 60 : 1;
	}

	static void Lookup(const ufuse_req *req, uint64_t parent, const char *name) {
		auto &srv = Of(req);
		std::string path;
		struct fuse_entry_out out;
		memset(&out, 0, sizeof(out));
		if (!PathOf(srv, parent, path)) {
			ufuse_req_reply_err(req, ENOENT);
			return;
		}
		auto child = Join(path, name);
		if (!Attr(srv, child, 0, &out.attr)) {
			ufuse_req_reply_err(req, ENOENT);
			return;
		}
		out.nodeid = out.attr.ino = srv.view->Ino(child);
		out.entry_valid = 1;
		out.attr_valid = AttrValid(out.attr);
		ufuse_req_reply(req, &out, sizeof(out));
	}
	static void Getattr(const ufuse_req *req, uint64_t ino, const struct fuse_getattr_in *) {
		auto &srv = Of(req);
		std::string path;
		struct fuse_attr_out out;
		memset(&out, 0, sizeof(out));
		if (!PathOf(srv, ino, path) || !Attr(srv, path, ino, &out.attr)) {
			ufuse_req_reply_err(req, ENOENT);
			return;
		}
		out.attr_valid = AttrValid(out.attr);
		ufuse_req_reply(req, &out, sizeof(out));
	}
	static void Opened(const ufuse_req *req, uint64_t ino, bool dir) {
		auto &srv = Of(req);
		std::string path;
		if (!PathOf(srv, ino, path) || (dir ? !srv.IsDirectory(path) : !srv.files.count(path))) {
			ufuse_req_reply_err(req, ENOENT);
			return;
		}
		struct fuse_open_out out;
		memset(&out, 0, sizeof(out));
		// The engines change files under the view, so the kernel keeps none of their pages
		out.open_flags = dir ? 0 : FOPEN_DIRECT_IO;
		ufuse_req_reply(req, &out, sizeof(out));
	}
	static void Open(const ufuse_req *req, uint64_t ino, const struct fuse_open_in *) {
		Opened(req, ino, false);
	}
	static void Opendir(const ufuse_req *req, uint64_t ino, const struct fuse_open_in *) {
		Opened(req, ino, true);
	}
	static void Read(const ufuse_req *req, uint64_t ino, const struct fuse_read_in *in) {
		auto &srv = Of(req);
		std::string path;
		auto f = PathOf(srv, ino, path) ? srv.files.find(path) : srv.files.end();
		if (f == srv.files.end()) {
			ufuse_req_reply_err(req, ENOENT);
			return;
		}
		auto &file = *f->second;
		auto &sb = srv.sb;
		uint64_t size = file.size;
		uint64_t want = in->offset >= size ? 0 : std::min<uint64_t>(in->size, size - in->offset);
		std::vector<uint8_t> data(want);
		try {
			for (uint64_t done = 0; done < want;) {
				uint64_t off = in->offset + done;
				uint64_t index = off / sb.extent_size;
				uint64_t within = off % sb.extent_size;
				uint64_t piece = std::min(want - done, sb.extent_size - within);
				if (index < file.extents.size()) {
					auto e = file.extents[index];
					srv.pool->GetMember(ExtentMember(e)).Read(data.data() + done, piece, ExtentOffset(sb, e) + within);
				}
				done += piece;
			}
		} catch (Error &err) {
			fprintf(stderr, "xnvme-fs: the view's read of '%s' failed: %s\n", path.c_str(), err.what());
			ufuse_req_reply_err(req, EIO);
			return;
		}
		ufuse_req_reply(req, data.data(), data.size());
	}
	static void Readdir(const ufuse_req *req, uint64_t ino, const struct fuse_read_in *in) {
		auto &srv = Of(req);
		std::string path;
		if (!PathOf(srv, ino, path) || !srv.IsDirectory(path)) {
			ufuse_req_reply_err(req, ENOTDIR);
			return;
		}
		std::vector<std::pair<std::string, EntryKind>> entries = {{".", EntryKind::DIRECTORY},
		                                                          {"..", EntryKind::DIRECTORY}};
		for (auto &kv : srv.Children(path)) {
			entries.push_back(kv);
		}
		std::vector<uint8_t> out(std::min<size_t>(in->size, UFUSE_MAX_WRITE));
		size_t used = 0;
		for (uint64_t i = in->offset; i < entries.size(); i++) {
			auto &name = entries[i].first;
			uint64_t node = name == "."    ? ino
			                : name == ".." ? srv.view->Ino(Parent(path))
			                               : srv.view->Ino(Join(path, name));
			uint32_t type = entries[i].second == EntryKind::DIRECTORY ? DT_DIR : DT_REG;
			size_t took = ufuse_dirent_add(out.data(), used, out.size(), name.c_str(), node, i + 1, type);
			if (!took) {
				break;
			}
			used += took;
		}
		ufuse_req_reply(req, out.data(), used);
	}
	static void Statfs(const ufuse_req *req, uint64_t) {
		auto &srv = Of(req);
		struct fuse_statfs_out out;
		memset(&out, 0, sizeof(out));
		uint64_t total = 0;
		uint64_t avail = 0;
		for (size_t m = 0; m < srv.used.size(); m++) {
			total += srv.used[m].size();
			avail += srv.free_count[m];
		}
		out.st.bsize = out.st.frsize = 4096;
		out.st.blocks = total * (srv.sb.extent_size / 4096);
		out.st.bfree = out.st.bavail = avail * (srv.sb.extent_size / 4096);
		out.st.files = srv.files.size();
		out.st.namelen = MAX_FILE_NAME;
		ufuse_req_reply(req, &out, sizeof(out));
	}
	static void Done(const ufuse_req *req, uint64_t, const struct fuse_release_in *) {
		ufuse_req_reply(req, nullptr, 0);
	}
	static void Flush(const ufuse_req *req, uint64_t, const struct fuse_flush_in *) {
		ufuse_req_reply(req, nullptr, 0);
	}

	static ufuse_ops Table() {
		ufuse_ops ops;
		memset(&ops, 0, sizeof(ops));
		ops.flags = UFUSE_READONLY;
		ops.want = FUSE_ASYNC_READ;
		ops.lookup = Lookup;
		ops.getattr = Getattr;
		ops.open = Open;
		ops.opendir = Opendir;
		ops.read = Read;
		ops.readdir = Readdir;
		ops.statfs = Statfs;
		ops.release = Done;
		ops.releasedir = Done;
		ops.flush = Flush;
		return ops;
	}
};

void Server::ViewChanged(const std::string &name) {
	if (!view) {
		return;
	}
	auto it = view->ino_of.find(name);
	if (it == view->ino_of.end()) {
		return;
	}
	// Attributes only: that takes no lock a request the view has yet to answer can hold, so this loop may send it
	int err = ufuse_notify_inval_inode(&view->session, it->second, -1, 0);
	if (err) {
		fprintf(stderr, "xnvme-fs: the view could not drop '%s''s attributes: %s\n", name.c_str(), strerror(-err));
	}
}

void Server::ServeView() {
	static const ufuse_ops ops = ViewOps::Table();
	auto &v = *view;
	ufuse_req req;
	auto n = ufuse_read(&v.session, v.buf.data(), v.buf.size(), &req);
	if (n == -EINTR || n == -EAGAIN || n == -ENOENT) {
		return;
	}
	if (n < 0) {
		fprintf(stderr, "xnvme-fs: the view at '%s' went away: %s\n", v.mountpoint.c_str(), strerror(int(-n)));
		CloseView();
		return;
	}
	int err = ufuse_dispatch(&v.session, &ops, this, v.buf.data(), size_t(n));
	if (err == -EPROTO && !v.session.minor) {
		fprintf(stderr, "xnvme-fs: the kernel's FUSE is older than protocol 7.%d; the view is closed\n",
		        UFUSE_MIN_MINOR);
		CloseView();
	}
}

} // namespace xnvme_fs
