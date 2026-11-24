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
	//! 'size' is the size the owner last reported. 'durable' is the size of the last commit, which WriteTable writes.
	uint64_t size = 0;
	uint64_t durable = 0;
	std::vector<ExtentAddr> extents;
	//! WriteTable writes the file's entry only when set. A commit or a rename sets it; FinishUnlink clears it.
	bool persistent = false;
	//! False once the file is removed from 'files'. The ServedFile exists until the last handle on it is closed.
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

	//! When set, Handle prints a line to stderr for each OPEN, CLOSE, LEASE, COMMIT, TRUNCATE, REMOVE, RENAME and
	//! INDEX request and for each request that fails with an Error, and Disconnect prints one for each connection.
	bool trace = false;

	//! Handles one request from connection `fd` and returns the response body, or an empty body for a WATCH that is
	//! answered later, whose response TakeReplies returns.
	std::vector<uint8_t> Handle(int fd, const std::vector<uint8_t> &request);
	//! Returns the responses that Changed queued for watches, each with its connection, for the caller to send.
	std::vector<std::pair<int, std::vector<uint8_t>>> TakeReplies();
	void Connect(int fd);
	//! Closes the connection's handles and ends its read leases. A file that the connection created and never
	//! committed stays in 'files'.
	void Disconnect(int fd);

	//! Mounts a read-only view of the pool at 'mountpoint', after unmounting what a server that was killed left there.
	void OpenView(const std::string &mountpoint);
	void CloseView();
	//! The view's descriptor to poll, or -1 without one.
	int ViewFd() const;
	//! Answers one request from the kernel for the view. Closes the view when reading a request fails, as it does
	//! after an unmount, and when the kernel's FUSE protocol is older than ufuse accepts.
	void ServeView();
	//! Tells the kernel to drop its cached attributes of 'name', whose size changed. Does nothing without a view.
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
	//! Increments the index version after a change to names, sizes or extents, and queues a reply to every watch.
	void Changed();
	//! Removes a file's extents past the first `keep`. An extent of a persistent file goes to 'pending_free', which
	//! WriteTable releases after the next table write; any other extent is released at once.
	void Drop(ServedFile &file, uint64_t keep);
	void TruncateFile(ServedFile &file, uint64_t size);
	//! Removes the file from 'files'. FinishUnlink, called once a table without the file is on the drive, releases
	//! the file's extents, or leaves them to the close of the file's last handle.
	void Unlink(const std::shared_ptr<ServedFile> &file);
	void FinishUnlink(const std::shared_ptr<ServedFile> &file);
	void CheckOwner(const ServedFile &file, int fd);
	//! Writes the directories and the persistent files, each file with its durable size and its extents, as the next
	//! generation of the table. Rethrows ENOSPC; on any other failure prints the error and exits the process.
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
