// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
// SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
//
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct xnvme_dev;

namespace xnvme_fs {

constexpr uint64_t SUPERBLOCK_SIZE = 4096;
constexpr uint32_t LAYOUT_VERSION = 2;
constexpr uint64_t DEFAULT_EXTENT_SIZE = 64ULL << 20;
constexpr uint64_t MIN_EXTENT_SIZE = 64ULL << 10;
constexpr uint64_t DATA_ALIGN = 1ULL << 20;
constexpr size_t MAX_MEMBERS = 16;
constexpr size_t MAX_MEMBER_PATH = 128;
constexpr size_t MAX_POOL_NAME = 64;
constexpr size_t MAX_FILE_NAME = 255;
//! The GPT partition type of a pool member, a04faa88-135a-4457-b8f6-db8bbaf344b8, as the GPT stores it.
constexpr uint8_t PARTITION_TYPE[16] = {0x88, 0xaa, 0x4f, 0xa0, 0x5a, 0x13, 0x57, 0x44,
                                        0xb8, 0xf6, 0xdb, 0x8b, 0xba, 0xf3, 0x44, 0xb8};

//! An extent's address: the member in the top byte, the extent's index on that member below.
using ExtentAddr = uint64_t;

inline ExtentAddr MakeExtent(uint32_t member, uint64_t index) {
	return (uint64_t(member) << 56) | index;
}
inline uint32_t ExtentMember(ExtentAddr e) {
	return uint32_t(e >> 56);
}
inline uint64_t ExtentIndex(ExtentAddr e) {
	return e & ((1ULL << 56) - 1);
}

//! The first SUPERBLOCK_SIZE bytes of every member, little-endian; the last four hold a CRC-32 of the rest.
//! Every member has the same slot size and data offset; only member 0's slots hold the table.
struct Superblock {
	uint32_t layout_version = LAYOUT_VERSION;
	uint32_t member_index = 0;
	uint8_t pool_uuid[16] = {0};
	uint64_t extent_size = DEFAULT_EXTENT_SIZE;
	//! Two slots of this size follow the superblock; the extent map table alternates between them.
	uint64_t slot_size = 0;
	uint64_t data_offset = 0;
	std::string pool_name;
	std::vector<std::string> members;

	void Encode(uint8_t *block) const;
	//! Throws Error naming `where` when the block is not a superblock this build can read.
	static Superblock Decode(const uint8_t *block, const std::string &where);
};

//! XNVME_FS_BACKEND, else "upcie".
std::string DefaultBackend();

//! How a process reaches the pool's members: the xNVMe backend and, for uPCIe, the HOMI server holding the
//! controllers. Every process using a pool on uPCIe is a client of that server; none opens a controller itself. The
//! other backends go through the kernel's nvme driver: 'io_uring' reads and writes a namespace's block device through
//! the page cache, 'io_uring_direct' the same without it, and 'io_uring_cmd' passes the NVMe commands through its
//! character device.
struct Access {
	std::string backend = DefaultBackend();
	uint32_t homi_id = 0;
};

enum class EntryKind : uint8_t { FILE = 1, DIRECTORY = 2 };

//! A file or directory as committed: its name, its durable size and its extents in the order of their file offsets.
struct Entry {
	std::string name;
	EntryKind kind = EntryKind::FILE;
	uint64_t size = 0;
	std::vector<ExtentAddr> extents;
};

//! The pool's namespace and extent maps as of one commit.
struct Table {
	uint64_t generation = 0;
	std::vector<Entry> entries;
};

//! Counts the commands of one request still in flight, which may be spread over several members, and keeps the first
//! error among them. 'done' is called, on the thread that polls, when the count drops to zero.
struct Completion {
	uint32_t outstanding = 0;
	int status = 0;
	//! What the first failure was: the device's status, or why a command could not be submitted
	std::string why;
	void (*done)(Completion *c, void *arg) = nullptr;
	void *arg = nullptr;
};

//! PacedWait keeps a moving average of the duration of a thread's waits per class. WaitClass gives the class of a
//! read or a write by its size, one per power of two from 4 KiB; FINISH_CLASS is the class of a flush and of any
//! other wait in Member::Finish.
constexpr int FINISH_CLASS = 8;
int WaitClass(uint64_t nbytes);
//! Polls with 'done' until it returns true. Unless XNVME_FS_POLL_MODE is 'spin', a wait that is not done at the first
//! poll first sleeps for half the average duration of this thread's waits of class 'cls', where that average is at
//! least 10 us, so the thread does not occupy its core for the whole wait on the drive.
void PacedWait(int cls, bool (*done)(void *arg), void *arg);

class Drive;

//! A pool member: one namespace or partition, or the drives of an md RAID 0 set, driven with NVM commands through
//! xNVMe; any byte range may be asked for. Each thread has a queue of its own on each drive, made on first use, so
//! threads share no queue, and a request larger than one command has its commands in flight together, up to the
//! queue's depth.
class Member {
public:
	//! 'path' is a controller held by HOMI, "0000:84:00.0", or under a kernel backend a namespace's block device,
	//! "/dev/nvme0n1", or its character device for 'io_uring_cmd', "/dev/ng0n1"; or partition N of its GPT,
	//! "0000:84:00.0/pN" or "/dev/ng0n1/pN", which must have the type PARTITION_TYPE; the member is then that
	//! partition alone. Or it is the drives of an md RAID 0 set joined by '+', "0000:82:00.0+0000:83:00.0" or
	//! "/dev/ng0n1+/dev/ng1n1", in any order, each a whole drive or a partition of the type Linux RAID or
	//! PARTITION_TYPE named as above; the member is then the set's array, which mdadm makes and inspects and xnvme fs
	//! maps to the drives as the kernel's md driver does. The constructor throws for a set of another level, a reshape,
	//! drives of unequal size and other md features.
	Member(const std::string &path, const Access &access);
	~Member();
	Member(const Member &) = delete;
	Member &operator=(const Member &) = delete;

	//! Starts reading 'nbytes' at 'offset' into 'buf' on the calling thread's queue; the bytes are there once the
	//! commands complete, and 'buf' must stay valid until then. Each command counts in 'c', if given.
	void BeginRead(void *buf, uint64_t nbytes, uint64_t offset, Completion *c = nullptr);
	//! Starts writing 'nbytes' at 'offset'; 'buf' is copied before this returns. A write covering a block in part
	//! reads and merges it, and finishes its own commands before returning.
	void BeginWrite(const void *buf, uint64_t nbytes, uint64_t offset, Completion *c = nullptr);
	void BeginFlush(Completion *c = nullptr);
	//! DMA memory from the HOMI serving the pool, which every member it serves can transfer to and from.
	void *AllocDma(uint64_t nbytes);
	void FreeDma(void *p);
	//! Makes the dma-buf 'fd', another device's memory, transferable under the range of this process's addresses at
	//! 'vaddr', the dma-buf's size, which the caller reserves and never touches; BeginReadDirect then takes addresses
	//! in it. Registered once, it serves every member the same HOMI, or the same process, holds. Throws ENOTSUP where
	//! the xNVMe built in cannot register dma-bufs.
	void RegisterDmabuf(int fd, void *vaddr);
	void UnregisterDmabuf(void *vaddr);
	//! Starts a transfer of whole blocks between the drive and 'dma', memory from AllocDma or of a registered dma-buf,
	//! which must stay valid until the commands complete. The bytes do not pass through a slot, so the CPU neither
	//! reads nor writes them, and memory the CPU cannot access, such as a device's behind a dma-buf, is used the same
	//! way.
	void BeginReadDirect(void *dma, uint64_t nbytes, uint64_t offset, Completion *c = nullptr);
	void BeginWriteDirect(const void *dma, uint64_t nbytes, uint64_t offset, Completion *c = nullptr);
	//! The most bytes one command transfers.
	uint64_t CommandBytes() const {
		return chunk;
	}
	//! Reaps what has completed on the calling thread's queues, without waiting.
	void Poke();
	//! Waits for everything the calling thread started on this member, and fails if any of it failed.
	void Finish(int cls = FINISH_CLASS);

	void Read(void *buf, uint64_t nbytes, uint64_t offset);
	void Write(const void *buf, uint64_t nbytes, uint64_t offset);
	void Sync();
	//! Makes a block-aligned range read as zeros, with Write Zeroes, or by writing zeros where the controller lacks
	//! it.
	void Zero(uint64_t offset, uint64_t nbytes);

	//! What this process's commands on the member did, for sampling: those in flight, counted until their thread
	//! reaps them, and the reads and writes completed.
	struct Stats {
		std::atomic<int64_t> in_flight {0};
		std::atomic<int64_t> reads_in_flight {0};
		std::atomic<int64_t> writes_in_flight {0};
		std::atomic<uint64_t> reads {0};
		std::atomic<uint64_t> writes {0};
		std::atomic<uint64_t> read_bytes {0};
		std::atomic<uint64_t> write_bytes {0};
		//! Reads that completed, by class and latency: bucket b counts those under 2^b microseconds, the last one the
		//! rest. The class is the reads in flight at submission, itself included (1, 2-4, 5-16, more), plus
		//! LATENCY_CLASSES / 2 when a write was in flight then; a completed write counts until its thread reaps it.
		static constexpr int LATENCY_CLASSES = 8, LATENCY_BUCKETS = 16;
		std::atomic<uint64_t> read_latency[LATENCY_CLASSES][LATENCY_BUCKETS] = {};
	};
	Stats stats;

	uint32_t BlockSize() const {
		return lba_nbytes;
	}
	const std::string &Path() const {
		return path;
	}
	uint64_t Size() const {
		return size;
	}

	//! Where a drive's share of the member starts on its namespace, a partition's first byte and then a stripe
	//! set's data offset, for a reader that addresses the drive itself.
	struct Place {
		std::string drive;
		uint64_t base;
	};
	//! The member's drives with their places, in the order the stripe takes them: chunk c of the member is chunk
	//! c / N of drive c % N. One drive unless striped.
	std::vector<Place> Places() const;
	//! The stripe set's chunk in bytes, or zero for one drive.
	uint64_t Stripe() const {
		return stripe;
	}

private:
	void OpenStripe(const std::vector<std::string> &names, const Access &access);
	//! Calls 'piece' for each run of [offset, offset + nbytes) within one chunk of the stripe set: the drive, the
	//! offset on it and the run's length, and how far into the range it starts.
	template <class F>
	void Split(uint64_t offset, uint64_t nbytes, F piece);

	std::string path;
	//! In the set's order, by each drive's role in its md superblock; one drive when the member is not a set.
	std::vector<std::unique_ptr<Drive>> drives;
	//! The set's chunk in bytes: chunk c of the array is chunk c / N of drive c % N. 0 for one drive.
	uint64_t stripe = 0;
	uint32_t lba_nbytes = 0;
	uint64_t chunk = 0;
	uint64_t size = 0;
};

struct MemberInfo {
	std::string path;
	uint64_t size;
	uint64_t extents;
};

//! One or more members, each carrying a superblock that names the pool and lists all of them.
class Pool {
public:
	//! Writes an empty table to member 0 and a superblock to every member. Throws EEXIST for a member that already
	//! holds a superblock, unless `overwrite`.
	static std::unique_ptr<Pool> Format(const std::string &name, const std::vector<std::string> &members,
	                                    const Access &access, uint64_t extent_size, bool overwrite);
	//! The superblock of one member, naming its pool and every member of it, as a mount reads it to open the pool.
	static Superblock Peek(const std::string &member, const Access &access);
	//! Reads and checks every member's superblock against the others and against `members`.
	static std::unique_ptr<Pool> Open(const std::string &name, const std::vector<std::string> &members,
	                                  const Access &access);

	const Superblock &Super() const {
		return superblock;
	}
	size_t MemberCount() const {
		return members.size();
	}
	Member &GetMember(size_t i) {
		return *members[i];
	}
	uint64_t ExtentsOn(size_t member) const;
	std::vector<MemberInfo> Members() const;

	//! The newest valid table of the two slots. Throws if neither holds one.
	Table ReadTable();
	//! Writes `table` to the slot its generation selects and flushes it; the caller advances the generation.
	void WriteTable(const Table &table);

private:
	Pool() = default;

	Superblock superblock;
	std::vector<std::unique_ptr<Member>> members;
};

//! Fails unless 'backend' is "upcie", "io_uring", "io_uring_direct" or "io_uring_cmd". Under "upcie", a controller
//! is held by HOMI and reached as its client, with no kernel driver in the data path.
void ValidateBackend(const std::string &backend);
//! Fails for an unknown backend, and for uPCIe without a HOMI id.
void ValidateAccess(const Access &access);

//! The drives a member names: itself, or each drive of a stripe set.
std::vector<std::string> MemberDrives(const std::string &member);

//! Fails for a file name the pool cannot hold: empty components, "." or "..", over MAX_FILE_NAME bytes.
void ValidateName(const std::string &name);

//! Byte offset of an extent on its member.
inline uint64_t ExtentOffset(const Superblock &sb, ExtentAddr e) {
	return sb.data_offset + ExtentIndex(e) * sb.extent_size;
}

} // namespace xnvme_fs
