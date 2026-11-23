// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
// SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
//
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "error.hpp"
#include "layout.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace xnvme_fs {

constexpr uint32_t PROTOCOL_VERSION = 1;
constexpr uint32_t MAX_FRAME = 64U << 20;
constexpr uint32_t MAX_LEASE = 4096;

//! A frame on the metadata socket is a u32 length and a body. A request body is an Op and its arguments; a
//! response body is an errno value, zero on success, then the results or, on failure, a message.
enum class Op : uint8_t {
	HELLO = 1,
	OPEN,
	CLOSE,
	LEASE,
	COMMIT,
	TRUNCATE,
	STAT,
	LIST,
	MKDIR,
	RMDIR,
	REMOVE,
	RENAME,
	INFO,
	//! For a reader outside the engines, one that reads files the engines write, as xal does for OpenDS: every
	//! file's extents under a read lease, a request answered once the index is stale, and the lease's end.
	INDEX,
	WATCH,
	RELEASE,
};

enum OpenFlags : uint32_t {
	OPEN_CREATE = 1,
	OPEN_TRUNCATE = 2,
	OPEN_EXCLUSIVE = 4,
};

//! What a client needs to address the pool's data itself.
struct Hello {
	uint64_t extent_size;
	uint64_t data_offset;
	std::vector<std::string> members;
};

struct Opened {
	uint64_t handle;
	uint64_t size;
	std::vector<ExtentAddr> extents;
};

struct Stat {
	//! Zero when the name does not exist.
	uint8_t kind;
	uint64_t size;
};

struct Listed {
	std::string name;
	EntryKind kind;
	//! A file's size as of its last commit; 0 for a directory.
	uint64_t size;
};

//! One page of a directory's entries, in the order of their names.
struct Listing {
	//! Advances whenever a name in the pool is added, removed or renamed, so a page of another version
	//! may skip or repeat an entry.
	uint64_t version;
	//! The entries after this page.
	uint64_t remaining;
	std::vector<Listed> entries;
};

//! The most entries the server returns for one LIST, whatever the client asks for.
constexpr uint32_t MAX_LIST = 4096;

//! A member as a reader addresses its drives itself.
struct IndexedMember {
	std::string path;
	uint64_t stripe;
	std::vector<Member::Place> places;
};

struct IndexedFile {
	std::string name;
	uint64_t size;
	//! The extents holding the file's bytes, in order, as many as its size needs.
	std::vector<ExtentAddr> extents;
};

//! The pool's files as one version of it has them, held by a read lease: while the lease is held, the server gives
//! no extent the index names to another file.
struct Index {
	uint64_t lease;
	uint64_t version;
	uint64_t extent_size;
	uint64_t data_offset;
	std::vector<IndexedMember> members;
	std::vector<std::string> directories;
	std::vector<IndexedFile> files;
};

struct PoolStats {
	std::string name;
	uint32_t layout_version;
	uint64_t extent_size;
	uint64_t generation;
	uint64_t files;
	std::vector<MemberInfo> members;
	std::vector<uint64_t> free_extents;
};

//! Sends one frame. Throws Error on a failed or short write.
void SendFrame(int fd, const std::vector<uint8_t> &body);
//! Receives one frame into `body`. False on a clean end of stream before a frame starts.
bool RecvFrame(int fd, std::vector<uint8_t> &body);

//! A connection to the pool's server. Calls are serialised, so one client may be shared between threads.
class Client {
public:
	//! Connects to `socket_path` and says hello for `pool`.
	Client(const std::string &socket_path, const std::string &pool);
	~Client();
	Client(const Client &) = delete;
	Client &operator=(const Client &) = delete;

	const Hello &Pool() const {
		return hello;
	}

	Opened Open(const std::string &name, uint32_t flags);
	void Close(uint64_t handle, uint64_t size);
	std::vector<ExtentAddr> Lease(uint64_t handle, uint32_t count);
	void Commit(uint64_t handle, uint64_t size);
	void Truncate(uint64_t handle, uint64_t size);
	Stat StatName(const std::string &name);
	Listing List(const std::string &directory, uint64_t offset, uint32_t max);
	void Mkdir(const std::string &name);
	void Rmdir(const std::string &name);
	void Remove(const std::string &name);
	void Rename(const std::string &from, const std::string &to);
	PoolStats Info();
	Index MakeIndex();
	//! Returns once the index of the lease is stale, with the version that made it so; a client waits on a
	//! connection of its own, which this holds meanwhile.
	uint64_t Watch(uint64_t lease);
	void Release(uint64_t lease);

	//! False once the connection has failed; a broken client is replaced, not repaired.
	bool Alive() const {
		return fd >= 0;
	}

private:
	std::vector<uint8_t> Call(const std::vector<uint8_t> &request);

	std::mutex lock;
	int fd = -1;
	Hello hello;
};

} // namespace xnvme_fs
