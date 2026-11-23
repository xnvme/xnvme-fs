// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
// SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
//
// SPDX-License-Identifier: BSD-3-Clause

#include "server.hpp"

#include "protocol.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <map>

namespace xnvme_fs {

Server::Server(std::unique_ptr<Pool> pool_p) : pool(std::move(pool_p)), sb(pool->Super()) {
	for (size_t m = 0; m < pool->MemberCount(); m++) {
		used.emplace_back(pool->ExtentsOn(m), false);
		free_count.push_back(pool->ExtentsOn(m));
	}
	auto table = pool->ReadTable();
	generation = table.generation;
	for (auto &e : table.entries) {
		if (e.kind == EntryKind::DIRECTORY) {
			dirs.insert(e.name);
			continue;
		}
		for (auto x : e.extents) {
			auto m = ExtentMember(x);
			auto i = ExtentIndex(x);
			if (m >= used.size() || i >= used[m].size() || used[m][i]) {
				Fail(EINVAL, "xnvme fs: the extent map of '%s' names extent %u:%llu, which is out of range or taken",
				     e.name.c_str(), m, (unsigned long long)i);
			}
			used[m][i] = true;
			free_count[m]--;
		}
		auto f = std::make_shared<ServedFile>();
		f->name = e.name;
		f->size = f->durable = e.size;
		f->extents = e.extents;
		f->persistent = true;
		files[e.name] = f;
	}
}

uint64_t Server::ExtentsFor(uint64_t size) const {
	return (size + sb.extent_size - 1) / sb.extent_size;
}

std::map<std::string, EntryKind> Server::Children(const std::string &dir) const {
	auto prefix = dir.empty() ? dir : dir + "/";
	std::map<std::string, EntryKind> children;
	auto add = [&](const std::string &name, EntryKind kind) {
		if (name.compare(0, prefix.size(), prefix) != 0 || name.size() == prefix.size()) {
			return;
		}
		auto rest = name.substr(prefix.size());
		auto slash = rest.find('/');
		if (slash == std::string::npos) {
			children[rest] = kind;
		} else {
			children[rest.substr(0, slash)] = EntryKind::DIRECTORY;
		}
	};
	for (auto it = files.lower_bound(prefix); it != files.end() && it->first.compare(0, prefix.size(), prefix) == 0;
	     ++it) {
		add(it->first, EntryKind::FILE);
	}
	for (auto it = dirs.lower_bound(prefix); it != dirs.end() && it->compare(0, prefix.size(), prefix) == 0; ++it) {
		add(*it, EntryKind::DIRECTORY);
	}
	return children;
}

bool Server::IsDirectory(const std::string &name) const {
	if (name.empty() || dirs.count(name)) {
		return true;
	}
	auto prefix = name + "/";
	auto f = files.lower_bound(prefix);
	if (f != files.end() && f->first.compare(0, prefix.size(), prefix) == 0) {
		return true;
	}
	auto d = dirs.lower_bound(prefix);
	return d != dirs.end() && d->compare(0, prefix.size(), prefix) == 0;
}

std::vector<ExtentAddr> Server::Allocate(uint64_t count) {
	uint64_t total = 0;
	for (auto n : free_count) {
		total += n;
	}
	if (count > total) {
		Fail(ENOSPC, "xnvme fs: pool '%s' has %llu free extents, %llu asked for", sb.pool_name.c_str(),
		     (unsigned long long)total, (unsigned long long)count);
	}
	std::vector<ExtentAddr> out;
	while (out.size() < count) {
		size_t m = 0;
		for (size_t k = 1; k < free_count.size(); k++) {
			if (free_count[k] > free_count[m]) {
				m = k;
			}
		}
		auto &bits = used[m];
		for (uint64_t i = 0; i < bits.size(); i++) {
			if (!bits[i]) {
				bits[i] = true;
				free_count[m]--;
				out.push_back(MakeExtent(uint32_t(m), i));
				break;
			}
		}
	}
	return out;
}

void Server::Release(ExtentAddr e) {
	if (!read_leases.empty()) {
		held.emplace_back(index_version, e);
		return;
	}
	Free(e);
}

void Server::Free(ExtentAddr e) {
	used[ExtentMember(e)][ExtentIndex(e)] = false;
	free_count[ExtentMember(e)]++;
}

void Server::FreeHeld() {
	uint64_t oldest = UINT64_MAX;
	for (auto &kv : read_leases) {
		oldest = std::min(oldest, kv.second.version);
	}
	// A lease taken at version v may name an extent released at version v or later
	auto keep = std::remove_if(held.begin(), held.end(), [&](const std::pair<uint64_t, ExtentAddr> &h) {
		if (h.first < oldest) {
			Free(h.second);
			return true;
		}
		return false;
	});
	held.erase(keep, held.end());
}

void Server::Changed() {
	index_version++;
	for (auto &kv : read_leases) {
		auto &lease = kv.second;
		if (lease.watching) {
			Writer w;
			w.I32(0);
			w.U64(index_version);
			replies.emplace_back(lease.fd, std::move(w.buf));
			lease.watching = false;
		}
	}
}

std::vector<std::pair<int, std::vector<uint8_t>>> Server::TakeReplies() {
	std::vector<std::pair<int, std::vector<uint8_t>>> out;
	out.swap(replies);
	return out;
}

void Server::Drop(ServedFile &file, uint64_t keep) {
	while (file.extents.size() > keep) {
		auto e = file.extents.back();
		file.extents.pop_back();
		if (file.persistent) {
			pending_free.push_back(e);
		} else {
			Release(e);
		}
	}
}

void Server::TruncateFile(ServedFile &file, uint64_t size) {
	Drop(file, ExtentsFor(size));
	file.size = size;
	file.durable = std::min(file.durable, size);
}

void Server::Unlink(const std::shared_ptr<ServedFile> &file) {
	files.erase(file->name);
	names_version++;
	file->linked = false;
}

void Server::FinishUnlink(const std::shared_ptr<ServedFile> &file) {
	// The table on the drive no longer names the file, so its extents need not wait for another write.
	file->persistent = false;
	if (file->opens == 0) {
		Drop(*file, 0);
	}
}

void Server::CheckOwner(const ServedFile &file, int fd) {
	if (file.owner != -1 && file.owner != fd) {
		Fail(EBUSY, "xnvme fs: '%s' is open in another client", file.name.c_str());
	}
}

void Server::WriteTable() {
	Table t;
	t.generation = generation + 1;
	for (auto &d : dirs) {
		Entry e;
		e.name = d;
		e.kind = EntryKind::DIRECTORY;
		t.entries.push_back(std::move(e));
	}
	for (auto &kv : files) {
		auto &f = *kv.second;
		if (!f.persistent) {
			continue;
		}
		Entry e;
		e.name = f.name;
		e.size = f.durable;
		e.extents = f.extents;
		t.entries.push_back(std::move(e));
	}
	try {
		pool->WriteTable(t);
	} catch (Error &err) {
		if (err.Code() == ENOSPC) {
			throw;
		}
		// Memory and drive now disagree on what is committed; restarting from the drive is the only safe course.
		fprintf(stderr, "xnvme-fs: committing the extent map table failed, exiting: %s\n", err.what());
		exit(2);
	}
	generation = t.generation;
	for (auto e : pending_free) {
		Release(e);
	}
	pending_free.clear();
}

void Server::Connect(int fd) {
	conns[fd] = Conn();
}

std::shared_ptr<ServedFile> Server::HandleOf(Conn &conn, uint64_t handle) {
	auto it = conn.handles.find(handle);
	if (it == conn.handles.end()) {
		Fail(EBADF, "xnvme fs: no open handle %llu", (unsigned long long)handle);
	}
	return it->second;
}

void Server::CloseHandle(Conn &conn, uint64_t handle) {
	auto file = HandleOf(conn, handle);
	conn.handles.erase(handle);
	if (--file->opens > 0) {
		return;
	}
	file->owner = -1;
	// Extents leased past the end are given back with the last handle.
	Drop(*file, file->linked ? ExtentsFor(file->size) : 0);
}

void Server::Disconnect(int fd) {
	auto it = conns.find(fd);
	if (it == conns.end()) {
		return;
	}
	auto &conn = it->second;
	if (trace) {
		fprintf(stderr, "xnvme-fs: [%d] disconnect with %zu handles\n", fd, conn.handles.size());
	}
	while (!conn.handles.empty()) {
		CloseHandle(conn, conn.handles.begin()->first);
	}
	for (auto l = read_leases.begin(); l != read_leases.end();) {
		l = l->second.fd == fd ? read_leases.erase(l) : std::next(l);
	}
	FreeHeld();
	// A file its client never committed stays, readable by the next client, as a closed file stays in the page cache:
	// what was written is on the drive, and only losing the server loses the map that names it
	conns.erase(it);
}

std::vector<uint8_t> Server::Handle(int fd, const std::vector<uint8_t> &request) {
	Writer w;
	w.I32(0);
	note.clear();
	deferred = false;
	try {
		Reader r(request.data(), request.size());
		Dispatch(fd, conns.at(fd), r, w);
		if (trace && !note.empty()) {
			fprintf(stderr, "xnvme-fs: [%d] %s\n", fd, note.c_str());
		}
	} catch (Error &err) {
		if (trace) {
			fprintf(stderr, "xnvme-fs: [%d] failed: %s\n", fd, err.what());
		}
		Writer e;
		e.I32(err.Code() ? err.Code() : EIO);
		e.Str(err.what());
		return e.buf;
	} catch (std::exception &err) {
		Writer e;
		e.I32(EIO);
		e.Str(std::string("xnvme-fs: ") + err.what());
		return e.buf;
	}
	if (deferred) {
		return {};
	}
	return w.buf;
}

void Server::Dispatch(int fd, Conn &conn, Reader &r, Writer &w) {
	auto op = Op(r.U8());
	if (op != Op::HELLO && !conn.hello) {
		Fail(EPROTO, "xnvme fs: a client says hello first");
	}
	switch (op) {
	case Op::HELLO: {
		auto version = r.U32();
		auto name = r.Str();
		if (version != PROTOCOL_VERSION) {
			Fail(EPROTO, "xnvme fs: client speaks protocol %u, the server speaks %u", version, PROTOCOL_VERSION);
		}
		if (name != sb.pool_name) {
			Fail(ENOENT, "xnvme fs: this server serves pool '%s', not '%s'", sb.pool_name.c_str(), name.c_str());
		}
		conn.hello = true;
		w.U64(sb.extent_size);
		w.U64(sb.data_offset);
		w.U32(uint32_t(sb.members.size()));
		for (auto &m : sb.members) {
			w.Str(m);
		}
		return;
	}
	case Op::OPEN: {
		auto name = r.Str();
		auto flags = r.U32();
		ValidateName(name);
		if (IsDirectory(name)) {
			Fail(EISDIR, "xnvme fs: '%s' is a directory", name.c_str());
		}
		auto it = files.find(name);
		std::shared_ptr<ServedFile> file;
		if (it == files.end()) {
			if (!(flags & OPEN_CREATE)) {
				Fail(ENOENT, "xnvme fs: no file '%s'", name.c_str());
			}
			file = std::make_shared<ServedFile>();
			file->name = name;
			files[name] = file;
			names_version++;
		} else {
			file = it->second;
			if ((flags & OPEN_CREATE) && (flags & OPEN_EXCLUSIVE)) {
				Fail(EEXIST, "xnvme fs: '%s' exists", name.c_str());
			}
			CheckOwner(*file, fd);
		}
		if (flags & OPEN_TRUNCATE) {
			TruncateFile(*file, 0);
		}
		if (it == files.end() || (flags & OPEN_TRUNCATE)) {
			Changed();
		}
		file->owner = fd;
		file->opens++;
		auto handle = next_handle++;
		conn.handles[handle] = file;
		w.U64(handle);
		w.U64(file->size);
		w.U64s(file->extents);
		note = Format("open '%s' flags %u -> handle %llu size %llu extents %zu", name.c_str(), flags,
		              (unsigned long long)handle, (unsigned long long)file->size, file->extents.size());
		return;
	}
	case Op::CLOSE: {
		auto handle = r.U64();
		auto size = r.U64();
		auto file = HandleOf(conn, handle);
		note = Format("close '%s' handle %llu size %llu (was %llu, durable %llu)", file->name.c_str(),
		              (unsigned long long)handle, (unsigned long long)size, (unsigned long long)file->size,
		              (unsigned long long)file->durable);
		file->size = size;
		ViewChanged(file->name);
		CloseHandle(conn, handle);
		Changed();
		return;
	}
	case Op::LEASE: {
		auto file = HandleOf(conn, r.U64());
		auto count = r.U32();
		if (count == 0 || count > MAX_LEASE) {
			Fail(EINVAL, "xnvme fs: a lease is 1 to %u extents", MAX_LEASE);
		}
		auto got = Allocate(count);
		// An extent holds what its last file left there; cleared, a range of the file nothing wrote reads as zeros,
		// as POSIX has it, and no file sees another's data
		try {
			for (auto e : got) {
				pool->GetMember(ExtentMember(e)).Zero(ExtentOffset(sb, e), sb.extent_size);
			}
		} catch (Error &) {
			for (auto e : got) {
				Release(e);
			}
			throw;
		}
		file->extents.insert(file->extents.end(), got.begin(), got.end());
		w.U64s(got);
		note = Format("lease '%s' %u -> extents %zu", file->name.c_str(), count, file->extents.size());
		return;
	}
	case Op::COMMIT: {
		auto file = HandleOf(conn, r.U64());
		// A size past the last extent is a hole at the tail, which reads as zeros.
		auto size = r.U64();
		file->size = file->durable = size;
		ViewChanged(file->name);
		if (file->linked) {
			file->persistent = true;
			WriteTable();
		}
		Changed();
		note =
		    Format("commit '%s' size %llu extents %zu%s generation %llu", file->name.c_str(), (unsigned long long)size,
		           file->extents.size(), file->linked ? "" : " (unlinked)", (unsigned long long)generation);
		return;
	}
	case Op::TRUNCATE: {
		auto file = HandleOf(conn, r.U64());
		auto size = r.U64();
		TruncateFile(*file, size);
		ViewChanged(file->name);
		Changed();
		note = Format("truncate '%s' size %llu durable %llu extents %zu", file->name.c_str(), (unsigned long long)size,
		              (unsigned long long)file->durable, file->extents.size());
		return;
	}
	case Op::STAT: {
		auto name = r.Str();
		auto it = files.find(name);
		if (it != files.end()) {
			w.U8(uint8_t(EntryKind::FILE));
			w.U64(it->second->size);
		} else if (IsDirectory(name)) {
			w.U8(uint8_t(EntryKind::DIRECTORY));
			w.U64(0);
		} else {
			w.U8(0);
			w.U64(0);
		}
		return;
	}
	case Op::LIST: {
		auto dir = r.Str();
		if (!IsDirectory(dir)) {
			Fail(ENOENT, "xnvme fs: no directory '%s'", dir.c_str());
		}
		auto offset = r.U64();
		auto max = std::min(r.U32(), MAX_LIST);
		auto children = Children(dir);
		auto prefix = dir.empty() ? dir : dir + "/";
		uint64_t first = std::min<uint64_t>(offset, children.size());
		uint64_t n = std::min<uint64_t>(max, children.size() - first);
		w.U64(names_version);
		w.U64(children.size() - first - n);
		w.U32(uint32_t(n));
		auto it = std::next(children.begin(), ptrdiff_t(first));
		for (uint64_t i = 0; i < n; i++, ++it) {
			auto f = it->second == EntryKind::FILE ? files.find(prefix + it->first) : files.end();
			w.Str(it->first);
			w.U8(uint8_t(it->second));
			w.U64(f != files.end() ? f->second->size : 0);
		}
		return;
	}
	case Op::MKDIR: {
		auto name = r.Str();
		ValidateName(name);
		if (files.count(name)) {
			Fail(EEXIST, "xnvme fs: '%s' is a file", name.c_str());
		}
		if (!dirs.count(name)) {
			dirs.insert(name);
			names_version++;
			WriteTable();
			Changed();
		}
		return;
	}
	case Op::RMDIR: {
		auto name = r.Str();
		ValidateName(name);
		if (files.count(name)) {
			Fail(ENOTDIR, "xnvme fs: '%s' is a file", name.c_str());
		}
		if (!IsDirectory(name)) {
			Fail(ENOENT, "xnvme fs: no directory '%s'", name.c_str());
		}
		auto prefix = name + "/";
		std::vector<std::shared_ptr<ServedFile>> doomed;
		for (auto it = files.lower_bound(prefix); it != files.end() && it->first.compare(0, prefix.size(), prefix) == 0;
		     ++it) {
			CheckOwner(*it->second, fd);
			doomed.push_back(it->second);
		}
		for (auto &f : doomed) {
			Unlink(f);
		}
		dirs.erase(name);
		for (auto it = dirs.lower_bound(prefix); it != dirs.end() && it->compare(0, prefix.size(), prefix) == 0;) {
			it = dirs.erase(it);
		}
		names_version++;
		WriteTable();
		for (auto &f : doomed) {
			FinishUnlink(f);
		}
		Changed();
		return;
	}
	case Op::REMOVE: {
		auto name = r.Str();
		auto it = files.find(name);
		if (it == files.end()) {
			Fail(IsDirectory(name) ? EISDIR : ENOENT, "xnvme fs: no file '%s'", name.c_str());
		}
		auto file = it->second;
		CheckOwner(*file, fd);
		Unlink(file);
		if (file->persistent) {
			WriteTable();
		}
		FinishUnlink(file);
		Changed();
		note =
		    Format("remove '%s' (open %d) generation %llu", name.c_str(), file->opens, (unsigned long long)generation);
		return;
	}
	case Op::RENAME: {
		auto from = r.Str();
		auto to = r.Str();
		ValidateName(to);
		auto it = files.find(from);
		if (it == files.end()) {
			Fail(IsDirectory(from) ? ENOTSUP : ENOENT, "xnvme fs: no file '%s' to rename", from.c_str());
		}
		auto file = it->second;
		CheckOwner(*file, fd);
		if (from == to) {
			return;
		}
		if (IsDirectory(to)) {
			Fail(EISDIR, "xnvme fs: '%s' is a directory", to.c_str());
		}
		std::shared_ptr<ServedFile> target;
		auto t = files.find(to);
		if (t != files.end()) {
			target = t->second;
			CheckOwner(*target, fd);
			Unlink(target);
		}
		files.erase(from);
		file->name = to;
		files[to] = file;
		names_version++;
		file->persistent = true;
		WriteTable();
		if (target) {
			FinishUnlink(target);
		}
		Changed();
		note = Format("rename '%s' -> '%s' durable %llu size %llu extents %zu generation %llu", from.c_str(),
		              to.c_str(), (unsigned long long)file->durable, (unsigned long long)file->size,
		              file->extents.size(), (unsigned long long)generation);
		return;
	}
	case Op::INFO: {
		w.Str(sb.pool_name);
		w.U32(sb.layout_version);
		w.U64(sb.extent_size);
		w.U64(generation);
		w.U64(files.size());
		auto members = pool->Members();
		w.U32(uint32_t(members.size()));
		for (size_t m = 0; m < members.size(); m++) {
			w.Str(members[m].path);
			w.U64(members[m].size);
			w.U64(members[m].extents);
			w.U64(free_count[m]);
		}
		return;
	}
	case Op::INDEX: {
		auto lease = next_lease++;
		read_leases[lease] = {fd, index_version, false};
		w.U64(lease);
		w.U64(index_version);
		w.U64(sb.extent_size);
		w.U64(sb.data_offset);
		w.U32(uint32_t(pool->MemberCount()));
		for (size_t m = 0; m < pool->MemberCount(); m++) {
			auto &member = pool->GetMember(m);
			w.Str(member.Path());
			w.U64(member.Stripe());
			auto places = member.Places();
			w.U32(uint32_t(places.size()));
			for (auto &p : places) {
				w.Str(p.drive);
				w.U64(p.base);
			}
		}
		w.U32(uint32_t(dirs.size()));
		for (auto &d : dirs) {
			w.Str(d);
		}
		w.U32(uint32_t(files.size()));
		for (auto &kv : files) {
			auto &f = *kv.second;
			auto n = std::min<uint64_t>(ExtentsFor(f.size), f.extents.size());
			w.Str(f.name);
			w.U64(f.size);
			w.U64s(std::vector<ExtentAddr>(f.extents.begin(), f.extents.begin() + n));
		}
		note = Format("index -> lease %llu version %llu files %zu", (unsigned long long)lease,
		              (unsigned long long)index_version, files.size());
		return;
	}
	case Op::WATCH: {
		auto id = r.U64();
		auto it = read_leases.find(id);
		if (it == read_leases.end() || it->second.fd != fd) {
			Fail(EINVAL, "xnvme fs: no read lease %llu on this connection", (unsigned long long)id);
		}
		if (it->second.version < index_version) {
			w.U64(index_version);
			return;
		}
		it->second.watching = true;
		deferred = true;
		return;
	}
	case Op::RELEASE: {
		auto id = r.U64();
		auto it = read_leases.find(id);
		if (it == read_leases.end() || it->second.fd != fd) {
			Fail(EINVAL, "xnvme fs: no read lease %llu on this connection", (unsigned long long)id);
		}
		read_leases.erase(it);
		FreeHeld();
		return;
	}
	}
	Fail(EPROTO, "xnvme fs: unknown request %u", unsigned(op));
}

} // namespace xnvme_fs
