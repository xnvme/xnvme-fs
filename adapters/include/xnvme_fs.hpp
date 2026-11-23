// SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
//
// SPDX-License-Identifier: MIT
//
// What the adapters share on top of yace/xnvme_fs.hpp, the C++ layer that yace generates from libxnvme_fs.h: a
// session that knows whether its connection to the server still works, and files that know which handles are one
// file of the pool. Header only; link libxnvme-fs. The classes here are in xnvme::fs, the generated ones in
// xnvme_fs. A failed call throws the generated layer's std::system_error, whose code() is the errno and whose
// what() is the name of the C function and the message xnvme_fs_errmsg() gave. A request is the generated
// xnvme_fs::req, started with File::Handle().submit().

#pragma once

#include "yace/xnvme_fs.hpp"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace xnvme {
namespace fs {

struct Entry {
	std::string name;
	bool directory;
};

struct PoolInfo {
	::xnvme_fs::info pool;
	std::vector<::xnvme_fs::member> members;
};

class File;

//! A process's connection to one pool's server. An engine keeps one per pool per process.
class Session : public std::enable_shared_from_this<Session> {
public:
	//! An empty 'backend' is XNVME_FS_BACKEND's, else upcie.
	Session(const std::string &pool_p, const std::string &socket_dir_p, const std::string &backend, uint32_t homi_id)
	    : pool(pool_p), socket_dir(socket_dir_p),
	      session(::xnvme_fs::connect(pool_p, socket_dir_p, backend.c_str(), homi_id)) {
	}
	Session(const Session &) = delete;
	Session &operator=(const Session &) = delete;

	std::shared_ptr<File> Open(const std::string &name, uint32_t flags);

	//! 'kind' is 0 where the pool has no such name; the size of a file open in this process is its live one.
	::xnvme_fs::stat Stat(const std::string &name) {
		return Checked([&]() { return session.stat(name); });
	}
	std::vector<Entry> List(const std::string &directory) {
		std::vector<Entry> entries;
		std::vector<::xnvme_fs::entry> page(PAGE);
		uint64_t remaining = 1, version = 0;
		while (remaining) {
			try {
				auto got = Checked([&]() { return session.list(directory, entries.size(), page, version); });
				for (auto &e : std::get<0>(got)) {
					entries.push_back({e.name, e.kind == XNVME_FS_KIND_DIRECTORY});
				}
				remaining = std::get<1>(got);
			} catch (const std::system_error &err) {
				// The names changed between two pages, so the offsets no longer name the same entries
				if (err.code().value() != ESTALE) {
					throw;
				}
				entries.clear();
				remaining = 1;
				version = 0;
			}
		}
		return entries;
	}
	void Mkdir(const std::string &name) {
		Checked([&]() { session.mkdir(name); });
	}
	void Rmdir(const std::string &name) {
		Checked([&]() { session.rmdir(name); });
	}
	//! The name's next file is another one: a File still open on the removed one no longer shares its Id().
	void Remove(const std::string &name) {
		std::lock_guard<std::mutex> guard(ids_lock);
		Checked([&]() { session.remove(name); });
		ids.erase(name);
	}
	void Rename(const std::string &from, const std::string &to) {
		std::lock_guard<std::mutex> guard(ids_lock);
		Checked([&]() { session.rename(from, to); });
		auto it = ids.find(from);
		if (it == ids.end()) {
			ids.erase(to);
			return;
		}
		auto id = it->second.lock();
		ids.erase(it);
		if (id) {
			*id = to;
			ids[to] = id;
		} else {
			ids.erase(to);
		}
	}
	PoolInfo Info() {
		PoolInfo info = {};
		info.pool = Checked([&]() { return session.info(); });
		std::vector<::xnvme_fs::member> page(PAGE);
		uint64_t remaining = 1;
		while (remaining) {
			auto got = Checked([&]() { return session.members(info.members.size(), page); });
			auto members = std::get<0>(got);
			info.members.insert(info.members.end(), members.begin(), members.end());
			remaining = std::get<1>(got);
		}
		return info;
	}

	//! False once a call has failed for the connection to the server; such a session is replaced, not repaired.
	bool Alive() const {
		return alive;
	}
	//! Runs 'call', which calls the generated layer, and passes its std::system_error on, after noting one that
	//! says the connection to the server is gone.
	template <class F>
	auto Checked(F call) -> decltype(call()) {
		try {
			return call();
		} catch (const std::system_error &err) {
			// What the library's client of the server's socket fails with, and with ENOTCONN ever after
			auto code = err.code().value();
			if (code == ENOTCONN || code == ECONNRESET || code == EPIPE || code == EPROTO) {
				alive = false;
			}
			throw;
		}
	}

	const std::string pool;
	const std::string socket_dir;

private:
	friend class File;

	//! The entries or members asked of the server in one call of xnvme_fs_list() or xnvme_fs_members()
	enum : uint32_t { PAGE = 256 };

	::xnvme_fs::session session;
	std::atomic<bool> alive {true};
	//! The name of each file open in this process, held by its Files and found by name here. libxnvme_fs.h gives
	//! every open a handle of its own and does not say which handles are one file.
	std::mutex ids_lock;
	std::unordered_map<std::string, std::weak_ptr<std::string>> ids;
};

//! A file open in this process: one handle, as a POSIX descriptor is. Safe to use from several threads; reads and
//! writes of different ranges run concurrently.
class File {
public:
	~File() {
		try {
			file.close();
		} catch (const std::system_error &) { // NOLINT: a destructor cannot report; Close() does
		}
		std::lock_guard<std::mutex> guard(session->ids_lock);
		auto name = *id;
		id.reset();
		auto it = session->ids.find(name);
		if (it != session->ids.end() && it->second.expired()) {
			session->ids.erase(it);
		}
	}
	File(const File &) = delete;
	File &operator=(const File &) = delete;

	//! Reads return the bytes read, fewer at the end of the file.
	uint64_t Read(void *buffer, uint64_t nbytes, uint64_t offset) {
		return session->Checked([&]() -> uint64_t {
			return file.pread({static_cast<std::byte *>(buffer), size_t(nbytes)}, offset).size();
		});
	}
	void Write(const void *buffer, uint64_t nbytes, uint64_t offset) {
		session->Checked([&]() { file.pwrite({static_cast<const std::byte *>(buffer), size_t(nbytes)}, offset); });
	}
	//! Makes the file's data and size durable.
	void Sync() {
		session->Checked([&]() { file.fsync(); });
	}
	void Truncate(uint64_t size) {
		session->Checked([&]() { file.ftruncate(size); });
	}
	uint64_t Size() {
		return session->Checked([&]() -> uint64_t { return file.size(); });
	}
	//! Writes the appends the file holds in memory and frees the handle, throwing if the write failed; the
	//! destructor does the same and cannot report.
	void Close() {
		session->Checked([&]() { file.close(); });
	}
	//! The same for every File open on one file of the pool, and for no other.
	const void *Id() const {
		return id.get();
	}
	//! The generated handle, which starts a request: Handle().submit(request, ...)
	::xnvme_fs::file &Handle() {
		return file;
	}

private:
	friend class Session;
	File(std::shared_ptr<Session> session_p, ::xnvme_fs::file file_p, std::shared_ptr<std::string> id_p)
	    : session(std::move(session_p)), file(std::move(file_p)), id(std::move(id_p)) {
	}

	// Declared before 'file', so the session is freed after the handle
	std::shared_ptr<Session> session;
	::xnvme_fs::file file;
	std::shared_ptr<std::string> id;
};

inline std::shared_ptr<File> Session::Open(const std::string &name, uint32_t flags) {
	std::lock_guard<std::mutex> guard(ids_lock);
	auto file = Checked([&]() { return session.open(name, flags); });
	auto &known = ids[name];
	auto id = known.lock();
	if (!id) {
		id = std::make_shared<std::string>(name);
		known = id;
	}
	return std::shared_ptr<File>(new File(shared_from_this(), std::move(file), std::move(id)));
}

} // namespace fs
} // namespace xnvme
