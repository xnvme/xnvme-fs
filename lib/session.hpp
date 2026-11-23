// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
// SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
//
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "protocol.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace xnvme_fs {

class Session;

enum class Io : uint8_t { READ, WRITE };

//! One read or write of a range of a file, in flight from File::Submit until 'done'. The submitting thread's queues
//! carry it, so that thread polls it: Poll reaps without waiting, Wait blocks. A write's buffer may be reused once
//! Submit returns; a read's must stay valid until the request is done, and the request must stay in place.
struct Request {
	Io op = Io::READ;
	void *buf = nullptr;
	uint64_t nbytes = 0;
	uint64_t offset = 0;
	//! A read straight into 'buf', memory the drives transfer to themselves, Member::AllocDma's or a registered
	//! dma-buf's, which the CPU never touches: whole blocks of the file's that lie on the drives, never a hole nor
	//! appends held in memory, else it fails with EINVAL. Past the file's end, the last block holds what the drive
	//! does, not zeros.
	bool direct = false;
	//! The caller's, untouched.
	void *tag = nullptr;

	bool done = false;
	//! 0, or the errno of the first part that failed.
	int status = 0;
	//! The bytes read or written; a read stops at the end of the file.
	uint64_t result = 0;

	Completion completion;
	//! The members the request's commands went to, which Poll and Wait poke.
	std::vector<Member *> members;
};

//! Reaps what has completed for 'r' without waiting; returns whether it is done.
bool Poll(Request &r);
//! Waits until 'r' is done, as PacedWait does, learning from waits on requests of its size.
void Wait(Request &r);

//! A file open in this process, shared by every handle an engine has on its name: one daemon handle, one view of
//! the size and the extents, as POSIX gives one inode to all of a process's descriptors. Safe to use from several
//! threads; reads and writes of different ranges run concurrently.
class File {
public:
	File(std::shared_ptr<Session> session, std::string name, Opened opened);
	~File();
	File(const File &) = delete;
	File &operator=(const File &) = delete;

	//! Starts 'r': splits its range at extent boundaries and starts every piece on its member's queue, so a range
	//! spanning members has all of them working at once. A write leases the extents it needs first. A hole, a range
	//! of the file no extent backs, reads as zeros. Never throws; a failure completes 'r' with its errno.
	void Submit(Request &r);
	//! Submit and Wait, for callers without anything to overlap. Reads return the bytes read, fewer at the end of
	//! the file; both throw on failure.
	uint64_t Read(void *buffer, uint64_t nbytes, uint64_t offset);
	void Write(const void *buffer, uint64_t nbytes, uint64_t offset);
	//! Writes what the append buffer holds, throwing if that fails; a handle's close calls it, since the file
	//! itself may outlive the handle and cannot report from its destructor.
	void Flush();
	//! Flushes the members, then commits the size and extent map through the pool's server.
	void Sync();
	void Truncate(uint64_t new_size);
	uint64_t Size();
	//! The name the file has now: Session::Renamed changes it, on whichever thread renames.
	std::string Name();

	//! A range of the file within one extent, or a hole past the last.
	struct Piece {
		ExtentAddr extent;
		bool hole;
		uint64_t within;
		uint64_t nbytes;
	};

private:
	//! Submit without tracing it, for Read and Write, which trace themselves.
	void Start(Request &r);
	//! Called under 'lock'.
	void LeaseTo(uint64_t end);
	//! Whether an append of 'nbytes' stays in the stage until a sync rather than being written as it comes: only
	//! with XNVME_FS_APPEND_BUFFER, and only below a command's size, which gains nothing from being held.
	bool Held(uint64_t nbytes) const;
	//! Bytes past 'size' the stage holds that are not on the drive yet. Called under 'lock'.
	uint64_t Pending() const;
	//! Loads the block holding the end of the file into the stage, if it is not there. Called under 'lock'.
	void LoadStage();
	//! Copies an append into the stage, writing the stage out each time it fills. Called under 'lock'.
	void Stage(const uint8_t *bytes, uint64_t nbytes);
	//! Writes what the stage holds as whole blocks, synchronously, so two appends to one block can never land out
	//! of order, then keeps the block holding the new end. Called under 'lock'.
	void WriteStage();
	//! Splits a range at extent boundaries. Called under 'lock'.
	std::vector<Piece> Pieces(uint64_t nbytes, uint64_t offset);

	friend class Session;

	std::shared_ptr<Session> session;
	uint64_t handle;
	std::mutex lock;
	//! Under 'lock'; Name() is for where it is not held.
	std::string name;
	uint64_t size;
	std::vector<ExtentAddr> extents;
	bool dirty = false;
	//! DMA memory of one command's size holding the file from 'stage_at', the start of the block holding the end,
	//! for 'stage_fill' bytes: that block as on the drive, then appends past 'size' not written yet, which only
	//! XNVME_FS_APPEND_BUFFER leaves there and a dying process loses. Zero past 'stage_fill', so a write pads with
	//! zeros. 'stage_at' is NO_STAGE when the block is not loaded. Under 'lock'.
	static constexpr uint64_t NO_STAGE = UINT64_MAX;
	uint8_t *stage = nullptr;
	uint64_t stage_nbytes = 0;
	uint64_t stage_at = NO_STAGE;
	uint64_t stage_fill = 0;
};

//! A process's connection to one pool's server, and the members it opened to do I/O on the pool itself. Engines
//! keep one per pool per process; the adapters' registries do that.
class Session : public std::enable_shared_from_this<Session> {
public:
	Session(const std::string &pool, const std::string &socket_path, const Access &access);
	~Session();

	//! Opens 'name', sharing the File already open in this process if there is one.
	std::shared_ptr<File> Open(const std::string &name, uint32_t flags);
	//! The File open in this process under 'name', or null. Its size is the live one; the server knows a size only as
	//! of the last commit, truncate or close.
	std::shared_ptr<File> Find(const std::string &name);
	//! Keeps the process's shared files in step with a removal or rename done through the client. A File still open
	//! under a name removed, or renamed onto, stays usable through its handles and is no longer found by name.
	void Forget(const std::string &name);
	//! As Forget, for every file under 'directory', which the server's RMDIR removed.
	void ForgetUnder(const std::string &directory);
	void Renamed(const std::string &from, const std::string &to);

	Member &MemberOf(ExtentAddr extent) {
		return *members[ExtentMember(extent)];
	}
	//! The member whose AllocDma serves the session's buffers: HOMI keeps one heap for all the members it serves.
	Member &DmaMember() {
		return *members[0];
	}
	uint64_t OffsetOf(ExtentAddr extent, uint64_t within) const {
		return client.Pool().data_offset + ExtentIndex(extent) * extent_size + within;
	}
	void SyncMembers();

	std::string pool;
	std::string socket_path;
	Client client;
	uint64_t extent_size;
	uint32_t block_size = 0;
	//! XNVME_FS_APPEND_BUFFER set to anything but 0: files hold appends in memory until a sync, up to one command.
	bool buffer_appends = false;

private:
	void Sample(const std::string &path);

	std::vector<std::unique_ptr<Member>> members;
	//! Runs with XNVME_FS_STATS set, until the session goes.
	std::thread sampler;
	std::atomic<bool> stop_sampling {false};
	std::mutex files_lock;
	std::unordered_map<std::string, std::weak_ptr<File>> files;
};

} // namespace xnvme_fs
