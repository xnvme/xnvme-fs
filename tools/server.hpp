// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
// SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
//
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "layout.hpp"
#include "wire.hpp"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace xnvme_fs {

//! The server's record of a file. Not named File: the library's client class is xnvme_fs::File, and xnvme-fs links
//! both, through 'xnvme fs cp'.
struct ServedFile {
	std::string name;
	//! What the owner last reported, and what the last commit made durable; only the latter reaches the drive.
	uint64_t size = 0;
	uint64_t durable = 0;
	std::vector<ExtentAddr> extents;
	//! In the table on the drive, or due there at the next table write. Set by a commit or a rename.
	bool persistent = false;
	//! In the namespace. A removed file stays alive while handles hold it.
	bool linked = true;
	int owner = -1;
	int opens = 0;
};

struct Conn {
	bool hello = false;
	std::map<uint64_t, std::shared_ptr<ServedFile>> handles;
};

//! The pool's metadata authority. Single-threaded: requests are handled one at a time, table writes included.
class Server {
public:
	explicit Server(std::unique_ptr<Pool> pool);
	~Server();

	//! Logs every request that changes a file, and every failure, to stderr.
	bool trace = false;

	//! Handles one request from connection `fd` and returns the response body, or nothing for a request answered
	//! later, a watch, whose response TakeReplies hands out.
	std::vector<uint8_t> Handle(int fd, const std::vector<uint8_t> &request);
	//! Responses to watches that came due, each with its connection, to send after every request.
	std::vector<std::pair<int, std::vector<uint8_t>>> TakeReplies();
	void Connect(int fd);
	//! Closes the connection's handles and removes the files it created but never made persistent.
	void Disconnect(int fd);

	//! Mounts a read-only view of the pool at 'mountpoint', clearing a mount a dead daemon left there.
	void OpenView(const std::string &mountpoint);
	void CloseView();
	//! The view's descriptor to poll, or -1 without one.
	int ViewFd() const;
	//! Answers one request from the kernel for the view; closes the view if it was unmounted from outside.
	void ServeView();
	//! Has the kernel drop the view's cached attributes of 'name', whose size changed; nothing without a view.
	void ViewChanged(const std::string &name);

private:
	void Dispatch(int fd, Conn &conn, Reader &r, Writer &w);
	std::shared_ptr<ServedFile> HandleOf(Conn &conn, uint64_t handle);
	void CloseHandle(Conn &conn, uint64_t handle);

	std::vector<ExtentAddr> Allocate(uint64_t count);
	//! Frees an extent, or holds it while a read lease older than its release may still name it.
	void Release(ExtentAddr e);
	void Free(ExtentAddr e);
	//! Frees the held extents no read lease can name any more.
	void FreeHeld();
	//! Moves the index version on after a change to names, sizes or extents, answering the watches it makes stale.
	void Changed();
	//! Drops a file's extents past the first `keep`. Extents the drive's table may name wait for the next write.
	void Drop(ServedFile &file, uint64_t keep);
	void TruncateFile(ServedFile &file, uint64_t size);
	//! Takes the file out of the namespace; FinishUnlink frees it once a table without it is on the drive.
	void Unlink(const std::shared_ptr<ServedFile> &file);
	void FinishUnlink(const std::shared_ptr<ServedFile> &file);
	void CheckOwner(const ServedFile &file, int fd);
	//! Commits the namespace and the durable extent maps; exits the process if the drive refuses.
	void WriteTable();

	bool IsDirectory(const std::string &name) const;
	//! The names directly in 'dir', with directories implied by deeper names among them.
	std::map<std::string, EntryKind> Children(const std::string &dir) const;
	uint64_t ExtentsFor(uint64_t size) const;

	std::unique_ptr<Pool> pool;
	Superblock sb;
	uint64_t generation = 0;
	std::map<std::string, std::shared_ptr<ServedFile>> files;
	std::set<std::string> dirs;
	std::vector<std::vector<bool>> used;
	std::vector<uint64_t> free_count;
	std::vector<ExtentAddr> pending_free;
	std::map<int, Conn> conns;
	uint64_t next_handle = 1;

	struct ReadLease {
		int fd;
		uint64_t version;
		bool watching;
	};
	uint64_t index_version = 0;
	//! Advances whenever a name is added, removed or renamed; a LIST returns it, so that a client paging
	//! through a directory learns that it changed. Zero is never a version.
	uint64_t names_version = 1;
	std::map<uint64_t, ReadLease> read_leases;
	uint64_t next_lease = 1;
	//! Released extents, each with the index version at its release, kept from reuse for older read leases.
	std::vector<std::pair<uint64_t, ExtentAddr>> held;
	std::vector<std::pair<int, std::vector<uint8_t>>> replies;
	//! Set by a request that is answered later.
	bool deferred = false;
	//! What the request being handled did, for the trace.
	std::string note;

	// Defined in view.cpp, so the FUSE structs stay out of this header
	struct ViewState;
	struct ViewOps;
	struct ViewDeleter {
		void operator()(ViewState *v) const;
	};
	std::unique_ptr<ViewState, ViewDeleter> view;
};

} // namespace xnvme_fs
