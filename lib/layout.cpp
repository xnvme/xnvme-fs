// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
// SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
//
// SPDX-License-Identifier: BSD-3-Clause

#include "layout.hpp"

#include "error.hpp"
#include "wire.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <exception>
#include <random>
#include <thread>
#include <unordered_map>

extern "C" {
#include <libxnvme.h>
#include <linux/raid/md_p.h>
#include <sys/prctl.h>
}

namespace xnvme_fs {

std::string Format(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	char small[512];
	int n = vsnprintf(small, sizeof(small), fmt, ap);
	va_end(ap);
	if (n < int(sizeof(small))) {
		return std::string(small, n < 0 ? 0 : size_t(n));
	}
	std::string s(size_t(n) + 1, '\0');
	va_start(ap, fmt);
	vsnprintf(&s[0], s.size(), fmt, ap);
	va_end(ap);
	s.resize(size_t(n));
	return s;
}

void Fail(int code, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	char buf[1024];
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	throw Error(code, buf);
}

uint32_t Crc32(const uint8_t *data, size_t n, uint32_t crc) {
	crc = ~crc;
	for (size_t i = 0; i < n; i++) {
		crc ^= data[i];
		for (int k = 0; k < 8; k++) {
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
		}
	}
	return ~crc;
}

static const uint8_t MAGIC[8] = {'X', 'N', 'V', 'M', 'E', 'F', 'S', 0};

static constexpr size_t OFF_VERSION = 8;
static constexpr size_t OFF_INDEX = 12;
static constexpr size_t OFF_COUNT = 16;
static constexpr size_t OFF_UUID = 24;
static constexpr size_t OFF_EXTENT = 40;
static constexpr size_t OFF_SLOT = 48;
static constexpr size_t OFF_DATA = 56;
static constexpr size_t OFF_NAME = 64;
static constexpr size_t OFF_MEMBERS = OFF_NAME + MAX_POOL_NAME;
static constexpr size_t OFF_CRC = SUPERBLOCK_SIZE - 4;
static_assert(OFF_MEMBERS + MAX_MEMBERS * MAX_MEMBER_PATH <= OFF_CRC, "members must fit before the checksum");

static void Put32(uint8_t *p, uint32_t v) {
	for (int i = 0; i < 4; i++) {
		p[i] = uint8_t(v >> (8 * i));
	}
}

static void Put64(uint8_t *p, uint64_t v) {
	for (int i = 0; i < 8; i++) {
		p[i] = uint8_t(v >> (8 * i));
	}
}

static uint16_t Get16(const uint8_t *p) {
	return uint16_t(p[0] | p[1] << 8);
}

static uint32_t Get32(const uint8_t *p) {
	return Reader(p, 4).U32();
}

static uint64_t Get64(const uint8_t *p) {
	return Reader(p, 8).U64();
}

static std::string FixedString(const uint8_t *p, size_t width) {
	return std::string(reinterpret_cast<const char *>(p), strnlen(reinterpret_cast<const char *>(p), width));
}

void Superblock::Encode(uint8_t *block) const {
	memset(block, 0, SUPERBLOCK_SIZE);
	memcpy(block, MAGIC, sizeof(MAGIC));
	Put32(block + OFF_VERSION, layout_version);
	Put32(block + OFF_INDEX, member_index);
	Put32(block + OFF_COUNT, uint32_t(members.size()));
	memcpy(block + OFF_UUID, pool_uuid, sizeof(pool_uuid));
	Put64(block + OFF_EXTENT, extent_size);
	Put64(block + OFF_SLOT, slot_size);
	Put64(block + OFF_DATA, data_offset);
	memcpy(block + OFF_NAME, pool_name.data(), pool_name.size());
	for (size_t i = 0; i < members.size(); i++) {
		memcpy(block + OFF_MEMBERS + i * MAX_MEMBER_PATH, members[i].data(), members[i].size());
	}
	Put32(block + OFF_CRC, Crc32(block, OFF_CRC));
}

Superblock Superblock::Decode(const uint8_t *block, const std::string &where) {
	if (memcmp(block, MAGIC, sizeof(MAGIC)) != 0) {
		Fail(EINVAL, "xnvme fs: %s is not a pool member: bad magic in its superblock", where.c_str());
	}
	Superblock sb;
	sb.layout_version = Get32(block + OFF_VERSION);
	if (sb.layout_version != LAYOUT_VERSION) {
		Fail(EINVAL, "xnvme fs: %s has layout version %u, this build reads version %u", where.c_str(), sb.layout_version,
		     LAYOUT_VERSION);
	}
	if (Get32(block + OFF_CRC) != Crc32(block, OFF_CRC)) {
		Fail(EINVAL, "xnvme fs: %s has a corrupt superblock: checksum mismatch", where.c_str());
	}
	sb.member_index = Get32(block + OFF_INDEX);
	uint32_t count = Get32(block + OFF_COUNT);
	if (count == 0 || count > MAX_MEMBERS || sb.member_index >= count) {
		Fail(EINVAL, "xnvme fs: %s has a corrupt superblock: member %u of %u", where.c_str(), sb.member_index, count);
	}
	memcpy(sb.pool_uuid, block + OFF_UUID, sizeof(sb.pool_uuid));
	sb.extent_size = Get64(block + OFF_EXTENT);
	sb.slot_size = Get64(block + OFF_SLOT);
	sb.data_offset = Get64(block + OFF_DATA);
	if (sb.extent_size < MIN_EXTENT_SIZE || sb.data_offset < SUPERBLOCK_SIZE + 2 * sb.slot_size) {
		Fail(EINVAL, "xnvme fs: %s has a corrupt superblock: inconsistent geometry", where.c_str());
	}
	sb.pool_name = FixedString(block + OFF_NAME, MAX_POOL_NAME);
	for (size_t i = 0; i < count; i++) {
		sb.members.push_back(FixedString(block + OFF_MEMBERS + i * MAX_MEMBER_PATH, MAX_MEMBER_PATH));
	}
	return sb;
}

// A table slot starts with this header; the CRC covers the header, its CRC field zeroed, and the payload.
static const uint8_t TABLE_MAGIC[8] = {'X', 'N', 'V', 'M', 'E', 'M', 'A', 'P'};
static constexpr size_t TABLE_HEADER = 64;
static constexpr size_t TOFF_CRC = 12;
static constexpr size_t TOFF_UUID = 16;
static constexpr size_t TOFF_GEN = 32;
static constexpr size_t TOFF_LEN = 40;

static uint64_t AlignUp(uint64_t v, uint64_t a) {
	return (v + a - 1) / a * a;
}

static std::vector<uint8_t> EncodeTable(const Table &table, const Superblock &sb) {
	Writer w;
	w.buf.resize(TABLE_HEADER);
	w.U32(uint32_t(table.entries.size()));
	for (auto &e : table.entries) {
		w.U8(uint8_t(e.kind));
		w.Str(e.name);
		w.U64(e.size);
		w.U64s(e.extents);
	}
	auto &buf = w.buf;
	uint64_t payload = buf.size() - TABLE_HEADER;
	memcpy(buf.data(), TABLE_MAGIC, sizeof(TABLE_MAGIC));
	Put32(buf.data() + 8, LAYOUT_VERSION);
	memcpy(buf.data() + TOFF_UUID, sb.pool_uuid, sizeof(sb.pool_uuid));
	Put64(buf.data() + TOFF_GEN, table.generation);
	Put64(buf.data() + TOFF_LEN, payload);
	Put32(buf.data() + TOFF_CRC, Crc32(buf.data(), buf.size()));
	buf.resize(AlignUp(buf.size(), SUPERBLOCK_SIZE), 0);
	if (buf.size() > sb.slot_size) {
		Fail(ENOSPC, "xnvme fs: the extent map table needs %zu bytes, a slot holds %llu", buf.size(),
		     (unsigned long long)sb.slot_size);
	}
	return buf;
}

//! False when the slot holds no table of this pool, or a torn or corrupt one.
static bool DecodeTable(std::vector<uint8_t> &slot, const Superblock &sb, Table &table) {
	uint8_t *h = slot.data();
	if (memcmp(h, TABLE_MAGIC, sizeof(TABLE_MAGIC)) != 0 || Get32(h + 8) != LAYOUT_VERSION ||
	    memcmp(h + TOFF_UUID, sb.pool_uuid, sizeof(sb.pool_uuid)) != 0) {
		return false;
	}
	uint64_t payload = Get64(h + TOFF_LEN);
	if (payload > slot.size() - TABLE_HEADER) {
		return false;
	}
	uint32_t crc = Get32(h + TOFF_CRC);
	Put32(h + TOFF_CRC, 0);
	if (Crc32(h, TABLE_HEADER + payload) != crc) {
		return false;
	}
	table.generation = Get64(h + TOFF_GEN);
	Reader r(h + TABLE_HEADER, payload);
	auto count = r.U32();
	table.entries.clear();
	for (uint32_t i = 0; i < count; i++) {
		Entry e;
		e.kind = EntryKind(r.U8());
		e.name = r.Str();
		e.size = r.U64();
		e.extents = r.U64s();
		table.entries.push_back(std::move(e));
	}
	return true;
}

std::string DefaultBackend() {
	auto env = getenv("XNVME_FS_BACKEND");
	return env && *env ? std::string(env) : std::string("upcie");
}

void ValidateBackend(const std::string &backend) {
	if (backend != "upcie" && backend != "io_uring" && backend != "io_uring_direct" && backend != "io_uring_cmd") {
		Fail(ENOTSUP,
		     "xnvme fs: backend '%s' is not available; 'upcie', 'io_uring', 'io_uring_direct' and "
		     "'io_uring_cmd' are",
		     backend.c_str());
	}
}

void ValidateAccess(const Access &access) {
	ValidateBackend(access.backend);
	if (access.backend == "upcie" && access.homi_id == 0) {
		Fail(EINVAL, "xnvme fs: uPCIe members are reached through HOMI; set the HOMI id");
	}
}

void ValidateName(const std::string &name) {
	if (name.empty() || name.size() > MAX_FILE_NAME) {
		Fail(ENAMETOOLONG, "xnvme fs: a file name is 1 to %zu bytes", MAX_FILE_NAME);
	}
	size_t start = 0;
	while (start <= name.size()) {
		auto slash = name.find('/', start);
		auto part = name.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
		if (part.empty() || part == "." || part == "..") {
			Fail(EINVAL, "xnvme fs: '%s' is not a file name the pool holds", name.c_str());
		}
		if (slash == std::string::npos) {
			break;
		}
		start = slash + 1;
	}
}

//! The GPT partition type Linux RAID, a19d880f-05fc-4d3b-a006-743f0f84911e, as the GPT stores it.
static constexpr uint8_t RAID_PARTITION_TYPE[16] = {0x0f, 0x88, 0x9d, 0xa1, 0xfc, 0x05, 0x3b, 0x4d,
                                                    0xa0, 0x06, 0x74, 0x3f, 0x0f, 0x84, 0x91, 0x1e};

//! One namespace, or one partition of it, opened through xNVMe and driven with NVM commands; any byte range may be
//! asked for. A member is one drive, or the drives of a stripe set.
class Drive {
public:
	//! 'raid' admits a partition of the type Linux RAID besides PARTITION_TYPE, as a drive of a stripe set.
	Drive(const std::string &path, const Access &access, Member::Stats &stats, bool raid);
	~Drive();
	Drive(const Drive &) = delete;
	Drive &operator=(const Drive &) = delete;

	void BeginRead(void *buf, uint64_t nbytes, uint64_t offset, Completion *c = nullptr);
	void BeginWrite(const void *buf, uint64_t nbytes, uint64_t offset, Completion *c = nullptr);
	void BeginFlush(Completion *c = nullptr);
	void *AllocDma(uint64_t nbytes);
	void FreeDma(void *p);
	void RegisterDmabuf(int fd, void *vaddr);
	void UnregisterDmabuf(void *vaddr);
	void BeginReadDirect(void *dma, uint64_t nbytes, uint64_t offset, Completion *c = nullptr);
	void BeginWriteDirect(const void *dma, uint64_t nbytes, uint64_t offset, Completion *c = nullptr);
	uint64_t CommandBytes() const {
		return chunk;
	}
	void Poke();
	void Finish(int cls = FINISH_CLASS);
	void Read(void *buf, uint64_t nbytes, uint64_t offset);
	void Write(const void *buf, uint64_t nbytes, uint64_t offset);
	void Zero(uint64_t offset, uint64_t nbytes);
	//! Moves the drive's start 'skip' bytes on and makes it 'nbytes' long, as a partition or a stripe set's data area.
	void Narrow(uint64_t skip, uint64_t nbytes);

	uint32_t BlockSize() const {
		return lba_nbytes;
	}
	const std::string &Path() const {
		return path;
	}
	uint64_t Size() const {
		return size;
	}
	uint64_t Base() const {
		return base;
	}

	struct ThreadQueue;
	//! Gives back a queue whose thread is exiting, once the commands it has in flight complete.
	void Release(ThreadQueue *queue);

private:
	//! The calling thread's queue on this drive, made on first use.
	ThreadQueue &Queue();
	void BeginDirect(uint8_t opc, void *dma, uint64_t nbytes, uint64_t offset, Completion *c);
	//! Narrows the drive to partition 'number' of the namespace's GPT.
	void OpenPartition(uint32_t number, bool raid);

	std::string path;
	xnvme_dev *dev = nullptr;
	//! Reached through the kernel's nvme driver, whose queues signal completions on a descriptor to sleep on.
	bool kernel = false;
	//! Through a block device, whose reads and writes complete with the bytes moved rather than an NVMe status.
	bool counted = false;
	uint32_t nsid = 0;
	uint32_t lba_nbytes = 0;
	//! The largest command: MDTS, and at most the slot size of a queue.
	uint64_t chunk = 0;
	uint64_t size = 0;
	//! Where the drive starts on the namespace: a partition's first byte, then a stripe set's data offset.
	uint64_t base = 0;
	//! Distinguishes drives in each thread's table of queues, where a freed drive's address may come back.
	uint64_t id = 0;
	Member::Stats *stats;
	std::mutex queues_lock;
	std::vector<ThreadQueue *> queues;
	//! Held by writes that cover a block only in part, between reading the block and writing it back.
	std::mutex rmw;
};

// A thread's queue on a drive: QUEUE_DEPTH commands of at most SLOT_MAX bytes, each with a slot of DMA-able
// memory, from the HOMI server's heap under uPCIe. As many slots as the queue is deep, so it never fills. The heap is
// one for every drive the server holds, and every thread of every client has a queue on each drive of its member, so
// a slot's memory is allocated when a command first needs it rather than with the queue.
static constexpr uint32_t QUEUE_DEPTH = 32;
static constexpr uint64_t SLOT_MAX = 256ULL << 10;
//! Slots allocated with the queue: a write covering a block in part holds one while it reads the block into another.
static constexpr uint32_t QUEUE_SLOTS_FIRST = 2;

struct Drive::ThreadQueue {
	struct Pending {
		ThreadQueue *queue;
		uint32_t slot;
		//! Where a read's bytes go, from where in the slot, and how many; null for writes and flushes.
		uint8_t *copy_to;
		uint64_t copy_from;
		uint64_t copy_nbytes;
		Completion *completion;
	};

	xnvme_queue *queue = nullptr;
	//! Reads and writes complete with the bytes moved, and the end of the device as a short count, not an error.
	bool counted = false;
	//! The member's base, added to every command's offset.
	uint64_t base = 0;
	xnvme_dev *dev = nullptr;
	//! Each slot's DMA memory, null until a command first needs it.
	uint8_t *slots[QUEUE_DEPTH] = {};
	uint64_t slot_nbytes = 0;
	std::vector<uint32_t> free_slots;
	Pending pending[QUEUE_DEPTH];
	//! Each slot's command, for the member's stats when it completes.
	uint8_t opc[QUEUE_DEPTH];
	uint64_t nbytes[QUEUE_DEPTH];
	//! A read's submission, in steady-clock nanoseconds, and its latency class.
	uint64_t submitted[QUEUE_DEPTH];
	uint8_t latency_class[QUEUE_DEPTH];
	Member::Stats *stats = nullptr;
	//! Set by a command that failed since the last Finish on this queue.
	bool failed = false;

	uint8_t *Slot(uint32_t slot) {
		return slots[slot];
	}
	void FreeSlots() {
		for (auto slot : slots) {
			if (slot) {
				xnvme_buf_free(dev, slot);
			}
		}
	}
};

static uint64_t SteadyNs() {
	return uint64_t(
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
	        .count());
}

//! The bytes a read or a write through a block device moved, which xNVMe puts in dwords 0 and 1 of the completion.
static uint64_t Moved(const struct xnvme_cmd_ctx *ctx) {
	return uint64_t(ctx->cpl.dw1) << 32 | ctx->cpl.dw0;
}

static void Completed(struct xnvme_cmd_ctx *ctx, void *arg) {
	auto p = static_cast<Drive::ThreadQueue::Pending *>(arg);
	bool failed = xnvme_cmd_ctx_cpl_status(ctx) != 0;
	auto opc = p->queue->opc[p->slot];
	bool moved_less = !failed && p->queue->counted &&
	                  (opc == XNVME_SPEC_NVM_READ_OPCODE || opc == XNVME_SPEC_NVM_WRITE_OPCODE) &&
	                  Moved(ctx) != p->queue->nbytes[p->slot];
	failed = failed || moved_less;
	// A command of a request reports to its Completion only; the queue's flag is for Finish, which such a request
	// never calls, so it would fail some later caller
	if (failed && !p->completion) {
		p->queue->failed = true;
	} else if (!failed && p->copy_to) {
		memcpy(p->copy_to, p->queue->Slot(p->slot) + p->copy_from, p->copy_nbytes);
	}
	p->queue->free_slots.push_back(p->slot);
	xnvme_queue_put_cmd_ctx(ctx->async.queue, ctx);
	auto &s = *p->queue->stats;
	s.in_flight--;
	if (p->queue->opc[p->slot] == XNVME_SPEC_NVM_READ_OPCODE) {
		s.reads_in_flight--;
		if (!failed) {
			s.reads++;
			s.read_bytes += p->queue->nbytes[p->slot];
			uint64_t us = (SteadyNs() - p->queue->submitted[p->slot]) / 1000;
			int bucket = 0;
			while (bucket < Member::Stats::LATENCY_BUCKETS - 1 && us >= (uint64_t(1) << bucket)) {
				bucket++;
			}
			s.read_latency[p->queue->latency_class[p->slot]][bucket]++;
		}
	} else if (p->queue->opc[p->slot] == XNVME_SPEC_NVM_WRITE_OPCODE) {
		s.writes_in_flight--;
		if (!failed) {
			s.writes++;
			s.write_bytes += p->queue->nbytes[p->slot];
		}
	}
	if (auto c = p->completion) {
		if (failed && !c->status) {
			c->status = EIO;
			char why[64];
			if (moved_less) {
				snprintf(why, sizeof(why), "transferred %u of %llu bytes", (unsigned)Moved(ctx),
				         (unsigned long long)p->queue->nbytes[p->slot]);
			} else {
				auto status = ctx->cpl.status;
				snprintf(why, sizeof(why), "NVMe status type 0x%x, code 0x%02x",
				         (unsigned)xnvme_bits_get(status, XNVME_SPEC_BASE_COMPLETION_QUEUE_ENTRY_STATUS_SCT),
				         (unsigned)xnvme_bits_get(status, XNVME_SPEC_BASE_COMPLETION_QUEUE_ENTRY_STATUS_SC));
			}
			c->why = why;
		}
		if (--c->outstanding == 0 && c->done) {
			c->done(c, c->arg);
		}
	}
}

//! Takes a free slot, polling the queue until one is; with 'buffer', a slot with its DMA memory. Once the heap has no
//! more to give, the queue goes on with the slots it has.
static uint32_t TakeSlot(Drive::ThreadQueue &q, bool buffer = true) {
	for (;;) {
		while (q.free_slots.empty()) {
			if (xnvme_queue_poke(q.queue, 0) < 0) {
				Fail(EIO, "xnvme fs: polling a queue failed");
			}
		}
		auto slot = q.free_slots.back();
		if (buffer && !q.slots[slot]) {
			q.slots[slot] = static_cast<uint8_t *>(xnvme_buf_alloc(q.dev, q.slot_nbytes));
			if (!q.slots[slot]) {
				auto &left = q.free_slots;
				left.erase(std::remove_if(left.begin(), left.end(), [&](uint32_t s) { return !q.slots[s]; }),
				           left.end());
				continue;
			}
		}
		q.free_slots.pop_back();
		return slot;
	}
}

static bool Spin() {
	static const bool spin = [] {
		auto poll = getenv("XNVME_FS_POLL_MODE");
		if (!poll || strcmp(poll, "paced") == 0) {
			return false;
		}
		if (strcmp(poll, "spin") != 0) {
			Fail(EINVAL, "xnvme fs: XNVME_FS_POLL_MODE is '%s', not 'paced' or 'spin'", poll);
		}
		return true;
	}();
	return spin;
}

int WaitClass(uint64_t nbytes) {
	int cls = 0;
	while (cls < FINISH_CLASS - 1 && nbytes > (uint64_t(4096) << cls)) {
		cls++;
	}
	return cls;
}

void PacedWait(int cls, bool (*done)(void *arg), void *arg) {
	if (done(arg)) {
		return;
	}
	constexpr uint64_t SLEEP_FROM_NS = 10000;
	//! Per class, how long this thread's waits took, as a moving average weighted 1/8 to the latest, in ns.
	static thread_local uint64_t mean_ns[FINISH_CLASS + 1];
	static thread_local bool slack_set = false;
	auto start = std::chrono::steady_clock::now();
	auto &mean = mean_ns[cls];
	if (!Spin() && mean >= SLEEP_FROM_NS) {
		if (!slack_set) {
			// The default 50 us of timer slack would outlast the sleep itself
			prctl(PR_SET_TIMERSLACK, 1000UL);
			slack_set = true;
		}
		std::this_thread::sleep_for(std::chrono::nanoseconds(mean / 2));
		// Done before the thread woke, by an unknown margin: learning the late wake as the wait would keep the thread
		// sleeping past a drive that has become fast, so the estimate halves instead
		if (done(arg)) {
			mean /= 2;
			return;
		}
	}
	while (!done(arg)) {
	}
	uint64_t took = uint64_t(
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
	mean = mean ? mean - mean / 8 + took / 8 : took;
}

//! Waits until nothing the calling thread started on 'q' is in flight.
static void Drain(Drive::ThreadQueue &q, int cls) {
	PacedWait(
	    cls,
	    [](void *arg) {
		    auto queue = static_cast<xnvme_queue *>(arg);
		    if (xnvme_queue_poke(queue, 0) < 0) {
			    Fail(EIO, "xnvme fs: polling a queue failed");
		    }
		    return xnvme_queue_get_outstanding(queue) == 0;
	    },
	    q.queue);
}

//! Runs 'issue', and on an exception waits out the commands it left in flight before passing it on: their
//! completions write to the caller's frame.
template <class F>
static void Issue(Drive::ThreadQueue &q, F issue) {
	try {
		issue();
	} catch (...) {
		xnvme_queue_drain(q.queue);
		throw;
	}
}

//! Submits a command on the queue, whose slot 'slot' and completion record the caller has filled. 'opc' is a read,
//! a write or a flush; reads and writes move 'nbytes' at byte 'offset' through the slot, or through 'data', DMA
//! memory of the caller's, when given.
static void Submit(Drive::ThreadQueue &q, uint32_t slot, uint8_t opc, uint32_t nsid, uint32_t lba_nbytes,
                   uint64_t offset, uint64_t nbytes, void *data = nullptr) {
	offset += q.base;
	auto ctx = xnvme_queue_get_cmd_ctx(q.queue);
	xnvme_cmd_ctx_set_cb(ctx, Completed, &q.pending[slot]);
	if (q.pending[slot].completion) {
		q.pending[slot].completion->outstanding++;
	}
	q.opc[slot] = opc;
	q.nbytes[slot] = nbytes;
	q.stats->in_flight++;
	if (opc == XNVME_SPEC_NVM_READ_OPCODE) {
		auto reads = ++q.stats->reads_in_flight;
		int c = reads <= 1 ? 0 : reads <= 4 ? 1 : reads <= 16 ? 2 : 3;
		q.latency_class[slot] = uint8_t(q.stats->writes_in_flight > 0 ? c + Member::Stats::LATENCY_CLASSES / 2 : c);
		q.submitted[slot] = SteadyNs();
	} else if (opc == XNVME_SPEC_NVM_WRITE_OPCODE) {
		q.stats->writes_in_flight++;
	}
	int err;
	if (opc == XNVME_SPEC_IO_FLUSH_OPCODE) {
		xnvme_cmd_prep_io_flush_args args = {};
		args.nsid = nsid;
		err = xnvme_cmd_prep_io_flush(&ctx->cmd, &args);
		if (!err) {
			err = xnvme_cmd_pass(ctx, nullptr, 0, nullptr, 0);
		}
	} else if (opc == XNVME_SPEC_NVM_WRITE_ZEROES_OPCODE) {
		xnvme_cmd_prep_nvm_write_zeroes_args args = {};
		args.nsid = nsid;
		args.slba = offset / lba_nbytes;
		args.nlb = uint16_t(nbytes / lba_nbytes - 1);
		err = xnvme_cmd_prep_nvm_write_zeroes(&ctx->cmd, &args);
		if (!err) {
			err = xnvme_cmd_pass(ctx, nullptr, 0, nullptr, 0);
		}
	} else {
		auto nlb = uint16_t(nbytes / lba_nbytes - 1);
		uint64_t slba = offset / lba_nbytes;
		auto buf = data ? data : q.Slot(slot);
		if (opc == XNVME_SPEC_NVM_WRITE_OPCODE) {
			xnvme_cmd_prep_nvm_write_args args = {};
			args.nsid = nsid;
			args.slba = slba;
			args.nlb = nlb;
			err = xnvme_cmd_prep_nvm_write(&ctx->cmd, &args);
		} else {
			xnvme_cmd_prep_nvm_read_args args = {};
			args.nsid = nsid;
			args.slba = slba;
			args.nlb = nlb;
			err = xnvme_cmd_prep_nvm_read(&ctx->cmd, &args);
		}
		if (!err) {
			err = xnvme_cmd_pass(ctx, buf, nbytes, nullptr, 0);
		}
	}
	if (err) {
		xnvme_queue_put_cmd_ctx(q.queue, ctx);
		q.free_slots.push_back(slot);
		if (q.pending[slot].completion) {
			q.pending[slot].completion->outstanding--;
		}
		q.stats->in_flight--;
		if (opc == XNVME_SPEC_NVM_READ_OPCODE) {
			q.stats->reads_in_flight--;
		} else if (opc == XNVME_SPEC_NVM_WRITE_OPCODE) {
			q.stats->writes_in_flight--;
		}
		Fail(EIO, "xnvme fs: submitting a command of %llu bytes at %llu failed: %d", (unsigned long long)nbytes,
		     (unsigned long long)offset, err);
	}
}

static std::atomic<uint64_t> next_drive_id {1};

// Engines start and stop threads, DuckDB with every database it opens, so a queue goes back to its drive when its
// thread exits rather than when the drive closes; a drive closing first takes its queues with it. The registry
// tells a thread's exit which of its drives are still open. It is never destroyed: a drive held by a static, such
// as an engine's registry of sessions, closes during static destruction, which may come after this file's.
struct DrivesOpen {
	std::mutex lock;
	std::unordered_map<uint64_t, Drive *> map;
};
static DrivesOpen &Drives() {
	static auto drives = new DrivesOpen();
	return *drives;
}

struct ThreadQueues {
	std::unordered_map<uint64_t, Drive::ThreadQueue *> map;
	~ThreadQueues() {
		auto &drives = Drives();
		std::lock_guard<std::mutex> guard(drives.lock);
		for (auto &kv : map) {
			auto d = drives.map.find(kv.first);
			if (d != drives.map.end()) {
				d->second->Release(kv.second);
			}
		}
	}
};
static thread_local ThreadQueues thread_queues;

Drive::Drive(const std::string &path_p, const Access &access, Member::Stats &stats_p, bool raid)
    : path(path_p), id(next_drive_id++), stats(&stats_p) {
	std::string controller = path;
	uint32_t partition = 0;
	// Only a last component of 'p' and digits names a partition: a device path such as /dev/disk/by-path/pci-... has
	// a '/p' of its own
	auto at = path.rfind("/p");
	if (at != std::string::npos && at + 2 < path.size() && isdigit((unsigned char)path[at + 2])) {
		char *end = nullptr;
		partition = uint32_t(strtoul(path.c_str() + at + 2, &end, 10));
		if (!partition || *end) {
			Fail(EINVAL, "xnvme fs: '%s' is not a controller or a controller's '/pN'", path.c_str());
		}
		controller = path.substr(0, at);
	}
	struct xnvme_opts opts = {};
	xnvme_opts_set_defaults(&opts);
	opts.rdwr = 1;
	if (access.backend == "upcie") {
		snprintf(opts.be, sizeof(opts.be), "%s", "upcie");
		opts.homi_id = access.homi_id;
	} else {
		// The synchronous interface must match the asynchronous one: NVMe ioctls beside buffered reads and writes
		// would pass the page cache by
		kernel = true;
		snprintf(opts.be, sizeof(opts.be), "%s", "linux");
		bool cmd = access.backend == "io_uring_cmd";
		snprintf(opts.async, sizeof(opts.async), "%s", cmd ? "io_uring_cmd" : "io_uring");
		snprintf(opts.sync, sizeof(opts.sync), "%s", cmd ? "nvme" : "block");
		snprintf(opts.admin, sizeof(opts.admin), "%s", cmd ? "nvme" : "block");
		opts.direct = access.backend == "io_uring_direct" ? 1 : 0;
		counted = !cmd;
	}
	int open_err = xnvme_dev_open_uri(controller.c_str(), &opts, &dev);
	if (open_err) {
		Fail(-open_err, "xnvme fs: cannot open member '%s' with the '%s' backend: %s", path.c_str(),
		     access.backend.c_str(), strerror(-open_err));
	}
	auto geo = xnvme_dev_get_geo(dev);
	nsid = xnvme_dev_get_nsid(dev);
	lba_nbytes = geo->lba_nbytes;
	size = geo->tbytes;
	// A command carries at most 65536 blocks (nlb is 16 bits, zero-based), the controller's MDTS and a slot
	chunk = std::min<uint64_t>(uint64_t(lba_nbytes) << 16, SLOT_MAX);
	if (geo->mdts_nbytes && geo->mdts_nbytes < chunk) {
		chunk = geo->mdts_nbytes;
	}
	chunk = chunk / lba_nbytes * lba_nbytes;
	if (partition) {
		try {
			OpenPartition(partition, raid);
		} catch (...) {
			// Not registered yet, so the thread's exit would not give the queue back
			thread_queues.map.erase(id);
			for (auto q : queues) {
				xnvme_queue_term(q->queue);
				q->FreeSlots();
				delete q;
			}
			xnvme_dev_close(dev);
			throw;
		}
	}
	auto &drives = Drives();
	std::lock_guard<std::mutex> guard(drives.lock);
	drives.map[id] = this;
}

void Drive::OpenPartition(uint32_t number, bool raid) {
	// The GPT's header is in block 1 whatever the block size; the UEFI specification gives its layout
	std::vector<uint8_t> header(lba_nbytes);
	Read(header.data(), lba_nbytes, lba_nbytes);
	auto h = header.data();
	uint32_t header_nbytes = Get32(h + 12);
	if (memcmp(h, "EFI PART", 8) || header_nbytes < 92 || header_nbytes > lba_nbytes) {
		Fail(EINVAL, "xnvme fs: '%s' holds no GPT", path.c_str());
	}
	uint32_t header_crc = Get32(h + 16);
	memset(h + 16, 0, 4);
	if (Crc32(h, header_nbytes) != header_crc) {
		Fail(EINVAL, "xnvme fs: the GPT header of '%s' is corrupt: checksum mismatch", path.c_str());
	}
	uint64_t entries_lba = Get64(h + 72);
	uint32_t count = Get32(h + 80);
	uint32_t entry_nbytes = Get32(h + 84);
	uint32_t entries_crc = Get32(h + 88);
	if (entry_nbytes < 128 || uint64_t(count) * entry_nbytes > (64ULL << 20)) {
		Fail(EINVAL, "xnvme fs: the GPT header of '%s' is corrupt: %u entries of %u bytes", path.c_str(), count,
		     entry_nbytes);
	}
	if (number > count) {
		Fail(ENOENT, "xnvme fs: the GPT of '%s' has %u entries", path.c_str(), count);
	}
	uint64_t array_nbytes = uint64_t(count) * entry_nbytes;
	std::vector<uint8_t> entries((array_nbytes + lba_nbytes - 1) / lba_nbytes * lba_nbytes);
	Read(entries.data(), entries.size(), entries_lba * lba_nbytes);
	if (Crc32(entries.data(), array_nbytes) != entries_crc) {
		Fail(EINVAL, "xnvme fs: the GPT entries of '%s' are corrupt: checksum mismatch", path.c_str());
	}
	auto e = entries.data() + uint64_t(number - 1) * entry_nbytes;
	if (raid && memcmp(e, RAID_PARTITION_TYPE, sizeof(RAID_PARTITION_TYPE)) &&
	    memcmp(e, PARTITION_TYPE, sizeof(PARTITION_TYPE))) {
		Fail(EINVAL,
		     "xnvme fs: '%s' is not a Linux RAID or an xnvme fs partition; its GPT type must be "
		     "a19d880f-05fc-4d3b-a006-743f0f84911e or a04faa88-135a-4457-b8f6-db8bbaf344b8",
		     path.c_str());
	}
	if (!raid && memcmp(e, PARTITION_TYPE, sizeof(PARTITION_TYPE))) {
		Fail(EINVAL,
		     "xnvme fs: '%s' is not an xnvme fs partition; its GPT type must be "
		     "a04faa88-135a-4457-b8f6-db8bbaf344b8",
		     path.c_str());
	}
	uint64_t first = Get64(e + 32);
	uint64_t last = Get64(e + 40);
	if (!first || last < first || (last + 1) * lba_nbytes > size) {
		Fail(EINVAL, "xnvme fs: the GPT entry of '%s' is corrupt: blocks %llu to %llu", path.c_str(),
		     (unsigned long long)first, (unsigned long long)last);
	}
	size = (last - first + 1) * lba_nbytes;
	Narrow(first * lba_nbytes, size);
}

void Drive::Narrow(uint64_t skip, uint64_t nbytes) {
	base += skip;
	size = nbytes;
	std::lock_guard<std::mutex> guard(queues_lock);
	for (auto q : queues) {
		q->base = base;
	}
}

Drive::~Drive() {
	auto &drives = Drives();
	std::lock_guard<std::mutex> guard(drives.lock);
	drives.map.erase(id);
	for (auto q : queues) {
		xnvme_queue_term(q->queue);
		q->FreeSlots();
		delete q;
	}
	if (dev) {
		xnvme_dev_close(dev);
	}
}

void Drive::Release(ThreadQueue *q) {
	std::lock_guard<std::mutex> guard(queues_lock);
	queues.erase(std::find(queues.begin(), queues.end(), q));
	// A command still in flight would land in a freed slot, and its request would never complete
	xnvme_queue_drain(q->queue);
	xnvme_queue_term(q->queue);
	q->FreeSlots();
	delete q;
}

Drive::ThreadQueue &Drive::Queue() {
	auto it = thread_queues.map.find(id);
	if (it != thread_queues.map.end()) {
		return *it->second;
	}
	std::unique_ptr<ThreadQueue> q(new ThreadQueue());
	q->dev = dev;
	q->counted = counted;
	q->slot_nbytes = chunk;
	q->base = base;
	q->stats = stats;
	for (uint32_t s = 0; s < QUEUE_SLOTS_FIRST; s++) {
		q->slots[s] = static_cast<uint8_t *>(xnvme_buf_alloc(dev, chunk));
		if (!q->slots[s]) {
			q->FreeSlots();
			Fail(ENOMEM, "xnvme fs: cannot allocate %llu bytes of DMA memory for a queue on '%s'",
			     (unsigned long long)(QUEUE_SLOTS_FIRST * chunk), path.c_str());
		}
	}
	int err = xnvme_queue_init(dev, QUEUE_DEPTH, 0, &q->queue);
	if (err) {
		q->FreeSlots();
		size_t held;
		{
			std::lock_guard<std::mutex> guard(queues_lock);
			held = queues.size();
		}
		// A controller out of I/O queues fails the creation with no status saying so
		Fail(-err,
		     "xnvme fs: cannot create a queue on '%s': %s; this process holds %zu there already, and the controller, "
		     "or HOMI's limit per client over all its drives, may allow no more: cap the threads using it",
		     path.c_str(), strerror(-err), held);
	}
	// Last in, first out: the slots with memory are taken before those without
	for (uint32_t s = QUEUE_DEPTH; s-- > 0;) {
		q->free_slots.push_back(s);
	}
	std::lock_guard<std::mutex> guard(queues_lock);
	queues.push_back(q.get());
	thread_queues.map[id] = q.get();
	return *q.release();
}

void Drive::BeginRead(void *buf, uint64_t nbytes, uint64_t offset, Completion *c) {
	auto &q = Queue();
	uint64_t start = offset / lba_nbytes * lba_nbytes;
	uint64_t end = AlignUp(offset + nbytes, lba_nbytes);
	auto out = static_cast<uint8_t *>(buf);
	Issue(q, [&]() {
		for (uint64_t at = start; at < end; at += chunk) {
			uint64_t n = std::min(chunk, end - at);
			uint64_t from = std::max(at, offset);
			uint64_t to = std::min(at + n, offset + nbytes);
			auto slot = TakeSlot(q);
			q.pending[slot] = {&q, slot, out + (from - offset), from - at, to - from, c};
			Submit(q, slot, XNVME_SPEC_NVM_READ_OPCODE, nsid, lba_nbytes, at, n);
		}
	});
}

void Drive::BeginWrite(const void *buf, uint64_t nbytes, uint64_t offset, Completion *c) {
	auto &q = Queue();
	uint64_t start = offset / lba_nbytes * lba_nbytes;
	uint64_t end = AlignUp(offset + nbytes, lba_nbytes);
	bool partial = start != offset || end != offset + nbytes;
	// A write covering a block in part holds this from reading the block until its write has landed, so two never
	// merge into the same block; it finishes its commands before returning for that reason.
	std::unique_lock<std::mutex> guard(rmw, std::defer_lock);
	if (partial) {
		guard.lock();
	}
	auto in = static_cast<const uint8_t *>(buf);
	Issue(q, [&]() {
		for (uint64_t at = start; at < end; at += chunk) {
			uint64_t n = std::min(chunk, end - at);
			uint64_t from = std::max(at, offset);
			uint64_t to = std::min(at + n, offset + nbytes);
			auto slot = TakeSlot(q);
			auto bytes = q.Slot(slot);
			// A block the write covers only in part is read into the slot first, so the rest of it survives
			uint64_t head = at;
			uint64_t tail = at + n - lba_nbytes;
			try {
				for (auto block : {head, tail}) {
					if ((block == head && from > at) || (block == tail && to < at + n)) {
						auto scratch = TakeSlot(q);
						q.pending[scratch] = {&q, scratch, bytes + (block - at), 0, lba_nbytes, nullptr};
						Submit(q, scratch, XNVME_SPEC_NVM_READ_OPCODE, nsid, lba_nbytes, block, lba_nbytes);
						Drain(q, WaitClass(lba_nbytes));
						if (q.failed) {
							q.failed = false;
							Fail(EIO, "xnvme fs: reading the block at %llu on '%s' to merge a write failed",
							     (unsigned long long)block, path.c_str());
						}
					}
				}
			} catch (...) {
				q.free_slots.push_back(slot);
				throw;
			}
			memcpy(bytes + (from - at), in + (from - offset), to - from);
			q.pending[slot] = {&q, slot, nullptr, 0, 0, c};
			Submit(q, slot, XNVME_SPEC_NVM_WRITE_OPCODE, nsid, lba_nbytes, at, n);
		}
		if (partial) {
			Drain(q, WaitClass(nbytes));
		}
	});
}

void *Drive::AllocDma(uint64_t nbytes) {
	auto p = xnvme_buf_alloc(dev, nbytes);
	if (!p) {
		Fail(ENOMEM, "xnvme fs: cannot allocate %llu bytes of DMA memory on '%s'", (unsigned long long)nbytes,
		     path.c_str());
	}
	return p;
}

void Drive::FreeDma(void *p) {
	xnvme_buf_free(dev, p);
}

void Drive::RegisterDmabuf(int fd, void *vaddr) {
#ifdef XNVME_FS_DMABUF
	int err = xnvme_buf_register_dmabuf(dev, fd, vaddr);
	if (err) {
		Fail(-err, "xnvme fs: cannot register a dma-buf with '%s': %s", path.c_str(), strerror(-err));
	}
#else
	(void)fd;
	(void)vaddr;
	Fail(ENOTSUP, "xnvme fs: the xNVMe built in does not register dma-bufs");
#endif
}

void Drive::UnregisterDmabuf(void *vaddr) {
#ifdef XNVME_FS_DMABUF
	int err = xnvme_buf_unregister(dev, vaddr);
	if (err) {
		Fail(-err, "xnvme fs: cannot unregister a dma-buf from '%s': %s", path.c_str(), strerror(-err));
	}
#else
	(void)vaddr;
	Fail(ENOTSUP, "xnvme fs: the xNVMe built in does not register dma-bufs");
#endif
}

void Drive::BeginDirect(uint8_t opc, void *dma, uint64_t nbytes, uint64_t offset, Completion *c) {
	if (offset % lba_nbytes || nbytes % lba_nbytes) {
		Fail(EINVAL, "xnvme fs: a direct transfer of %llu bytes at %llu on '%s' is not whole blocks",
		     (unsigned long long)nbytes, (unsigned long long)offset, path.c_str());
	}
	auto &q = Queue();
	Issue(q, [&]() {
		for (uint64_t at = 0; at < nbytes; at += chunk) {
			auto slot = TakeSlot(q, false);
			q.pending[slot] = {&q, slot, nullptr, 0, 0, c};
			Submit(q, slot, opc, nsid, lba_nbytes, offset + at, std::min(chunk, nbytes - at),
			       static_cast<uint8_t *>(dma) + at);
		}
	});
}

void Drive::BeginReadDirect(void *dma, uint64_t nbytes, uint64_t offset, Completion *c) {
	BeginDirect(XNVME_SPEC_NVM_READ_OPCODE, dma, nbytes, offset, c);
}

void Drive::BeginWriteDirect(const void *dma, uint64_t nbytes, uint64_t offset, Completion *c) {
	BeginDirect(XNVME_SPEC_NVM_WRITE_OPCODE, const_cast<void *>(dma), nbytes, offset, c);
}

void Drive::BeginFlush(Completion *c) {
	auto &q = Queue();
	Issue(q, [&]() {
		auto slot = TakeSlot(q, false);
		q.pending[slot] = {&q, slot, nullptr, 0, 0, c};
		Submit(q, slot, XNVME_SPEC_IO_FLUSH_OPCODE, nsid, lba_nbytes, 0, 0);
	});
}

void Drive::Poke() {
	auto &q = Queue();
	if (xnvme_queue_poke(q.queue, 0) < 0) {
		Fail(EIO, "xnvme fs: polling a queue on '%s' failed", path.c_str());
	}
}

void Drive::Finish(int cls) {
	auto &q = Queue();
	Drain(q, cls);
	if (q.failed) {
		q.failed = false;
		Fail(EIO, "xnvme fs: a command on '%s' failed", path.c_str());
	}
}

void Drive::Zero(uint64_t offset, uint64_t nbytes) {
	auto &q = Queue();
	// A command clears at most 65536 blocks, nlb being 16 bits and zero-based
	uint64_t most = uint64_t(lba_nbytes) << 16;
	try {
		Issue(q, [&]() {
			for (uint64_t at = offset; at < offset + nbytes; at += most) {
				auto slot = TakeSlot(q, false);
				q.pending[slot] = {&q, slot, nullptr, 0, 0, nullptr};
				Submit(q, slot, XNVME_SPEC_NVM_WRITE_ZEROES_OPCODE, nsid, lba_nbytes, at,
				       std::min(most, offset + nbytes - at));
			}
		});
		Finish();
	} catch (Error &) {
		// A controller without Write Zeroes gets the zeros written
		std::vector<uint8_t> zeros(chunk, 0);
		for (uint64_t at = offset; at < offset + nbytes; at += chunk) {
			BeginWrite(zeros.data(), std::min(chunk, offset + nbytes - at), at);
		}
		Finish();
	}
}

void Drive::Read(void *buf, uint64_t nbytes, uint64_t offset) {
	BeginRead(buf, nbytes, offset);
	Finish(WaitClass(nbytes));
}

void Drive::Write(const void *buf, uint64_t nbytes, uint64_t offset) {
	BeginWrite(buf, nbytes, offset);
	Finish(WaitClass(nbytes));
}

// md's superblock of format 1.2 lies 4 KiB into each drive of a set; linux/raid/md_p.h gives its layout.
static constexpr uint64_t MD_SUPER_OFFSET = 4096;
#define MD_FIELD(f) offsetof(struct mdp_superblock_1, f)

//! What one drive's md superblock says about the set and the drive's place in it.
struct MdDrive {
	uint8_t set_uuid[16];
	uint32_t level, raid_disks, chunk_sectors, role;
	uint64_t events, data_offset, data_size;
};

//! The checksum md keeps in sb_csum: the sum of the superblock's little-endian 32-bit words, its own field zeroed,
//! folded to 32 bits.
static uint32_t MdChecksum(const uint8_t *sb, uint32_t nbytes) {
	uint64_t sum = 0;
	for (uint32_t at = 0; at + 4 <= nbytes; at += 4) {
		if (at != MD_FIELD(sb_csum)) {
			sum += Get32(sb + at);
		}
	}
	if (nbytes % 4 == 2) {
		sum += uint32_t(sb[nbytes - 2]) | uint32_t(sb[nbytes - 1]) << 8;
	}
	return uint32_t((sum & 0xffffffff) + (sum >> 32));
}

static MdDrive ReadMdSuperblock(Drive &drive) {
	auto where = drive.Path().c_str();
	uint8_t sb[SUPERBLOCK_SIZE];
	if (drive.Size() < MD_SUPER_OFFSET + sizeof(sb)) {
		Fail(EINVAL, "xnvme fs: '%s' is too small to hold an md superblock", where);
	}
	drive.Read(sb, sizeof(sb), MD_SUPER_OFFSET);
	if (Get32(sb + MD_FIELD(magic)) != MD_SB_MAGIC || Get32(sb + MD_FIELD(major_version)) != 1 ||
	    Get64(sb + MD_FIELD(super_offset)) != MD_SUPER_OFFSET / 512) {
		Fail(EINVAL, "xnvme fs: '%s' holds no md superblock of format 1.2", where);
	}
	uint32_t max_dev = Get32(sb + MD_FIELD(max_dev));
	uint32_t nbytes = uint32_t(sizeof(struct mdp_superblock_1)) + 2 * max_dev;
	if (max_dev == 0 || nbytes > sizeof(sb)) {
		Fail(EINVAL, "xnvme fs: the md superblock of '%s' is corrupt: %u devices", where, max_dev);
	}
	if (Get32(sb + MD_FIELD(sb_csum)) != MdChecksum(sb, nbytes)) {
		Fail(EINVAL, "xnvme fs: the md superblock of '%s' is corrupt: checksum mismatch", where);
	}
	MdDrive md;
	memcpy(md.set_uuid, sb + MD_FIELD(set_uuid), sizeof(md.set_uuid));
	md.level = Get32(sb + MD_FIELD(level));
	md.raid_disks = Get32(sb + MD_FIELD(raid_disks));
	md.chunk_sectors = Get32(sb + MD_FIELD(chunksize));
	md.events = Get64(sb + MD_FIELD(events));
	md.data_offset = Get64(sb + MD_FIELD(data_offset));
	md.data_size = Get64(sb + MD_FIELD(data_size));
	uint32_t features = Get32(sb + MD_FIELD(feature_map));
	uint32_t dev_number = Get32(sb + MD_FIELD(dev_number));
	md.role = dev_number < max_dev ? Get16(sb + MD_FIELD(dev_roles) + 2 * dev_number) : MD_DISK_ROLE_SPARE;
	if (md.level != 0) {
		Fail(ENOTSUP, "xnvme fs: '%s' belongs to an md set of level %d; only RAID 0 is striped", where, int(md.level));
	}
	// Any feature but RAID 0's layout field changes where data lies, as a reshape in progress does
	if (features & ~uint32_t(MD_FEATURE_RAID0_LAYOUT)) {
		Fail(ENOTSUP, "xnvme fs: '%s' belongs to an md set with features 0x%x, which xnvme fs does not map", where,
		     features);
	}
	if (md.role >= md.raid_disks) {
		Fail(EINVAL, "xnvme fs: '%s' is not an active drive of its md set", where);
	}
	if (md.chunk_sectors == 0 || (uint64_t(md.chunk_sectors) * 512) % drive.BlockSize()) {
		Fail(EINVAL, "xnvme fs: the md set of '%s' has a chunk of %u sectors, not whole blocks", where,
		     md.chunk_sectors);
	}
	if ((md.data_offset + md.data_size) * 512 > drive.Size() || (md.data_offset * 512) % drive.BlockSize()) {
		Fail(EINVAL, "xnvme fs: the md superblock of '%s' is corrupt: data at sector %llu, %llu sectors", where,
		     (unsigned long long)md.data_offset, (unsigned long long)md.data_size);
	}
	return md;
}

std::vector<std::string> MemberDrives(const std::string &path) {
	std::vector<std::string> names;
	size_t start = 0;
	while (true) {
		auto plus = path.find('+', start);
		names.push_back(path.substr(start, plus == std::string::npos ? std::string::npos : plus - start));
		if (plus == std::string::npos) {
			return names;
		}
		start = plus + 1;
	}
}

Member::Member(const std::string &path_p, const Access &access) : path(path_p) {
	ValidateAccess(access);
	auto names = MemberDrives(path);
	if (names.size() == 1) {
		drives.push_back(std::unique_ptr<Drive>(new Drive(path, access, stats, false)));
	} else {
		OpenStripe(names, access);
	}
	lba_nbytes = drives[0]->BlockSize();
	chunk = drives[0]->CommandBytes();
	for (auto &d : drives) {
		chunk = std::min(chunk, d->CommandBytes());
	}
	size = drives[0]->Size() * drives.size();
}

void Member::OpenStripe(const std::vector<std::string> &names, const Access &access) {
	std::vector<std::unique_ptr<Drive>> opened;
	std::vector<MdDrive> md;
	for (auto &name : names) {
		if (name.empty() || (access.backend == "upcie" && name.find('/') == 0)) {
			Fail(EINVAL, "xnvme fs: member '%s' is not drives joined by '+'", path.c_str());
		}
		opened.push_back(std::unique_ptr<Drive>(new Drive(name, access, stats, true)));
		md.push_back(ReadMdSuperblock(*opened.back()));
	}
	auto &first = md[0];
	if (first.raid_disks != names.size()) {
		Fail(EINVAL, "xnvme fs: member '%s' names %zu drives, its md set has %u", path.c_str(), names.size(),
		     first.raid_disks);
	}
	drives.resize(names.size());
	// md rounds each drive's data down to whole chunks; a set whose drives then differ in size has several zones,
	// each striped over fewer drives, which xnvme fs does not map
	uint64_t chunk_sectors = first.chunk_sectors;
	uint64_t sectors = first.data_size / chunk_sectors * chunk_sectors;
	uint32_t block = opened[0]->BlockSize();
	for (size_t i = 0; i < names.size(); i++) {
		auto where = names[i].c_str();
		if (memcmp(md[i].set_uuid, first.set_uuid, sizeof(first.set_uuid)) || md[i].raid_disks != first.raid_disks ||
		    md[i].chunk_sectors != first.chunk_sectors) {
			Fail(EINVAL, "xnvme fs: '%s' and '%s' belong to different md sets", names[0].c_str(), where);
		}
		if (md[i].events != first.events) {
			Fail(EINVAL, "xnvme fs: the md superblocks of '%s' and '%s' disagree: events %llu and %llu",
			     names[0].c_str(), where, (unsigned long long)first.events, (unsigned long long)md[i].events);
		}
		if (md[i].data_size / chunk_sectors * chunk_sectors != sectors) {
			Fail(ENOTSUP, "xnvme fs: the drives of md set '%s' differ in size, which xnvme fs does not stripe",
			     path.c_str());
		}
		if (opened[i]->BlockSize() != block) {
			Fail(EINVAL, "xnvme fs: the drives of md set '%s' differ in block size", path.c_str());
		}
		if (drives[md[i].role]) {
			Fail(EINVAL, "xnvme fs: '%s' and '%s' hold the same place in their md set",
			     drives[md[i].role]->Path().c_str(), where);
		}
		opened[i]->Narrow(md[i].data_offset * 512, sectors * 512);
		drives[md[i].role] = std::move(opened[i]);
	}
	stripe = chunk_sectors * 512;
}

Member::~Member() = default;

std::vector<Member::Place> Member::Places() const {
	std::vector<Place> places;
	for (auto &d : drives) {
		places.push_back({d->Path(), d->Base()});
	}
	return places;
}

template <class F>
void Member::Split(uint64_t offset, uint64_t nbytes, F piece) {
	uint64_t done = 0;
	try {
		while (done < nbytes) {
			uint64_t at = offset + done;
			uint64_t c = at / stripe;
			uint64_t within = at % stripe;
			uint64_t n = std::min(nbytes - done, stripe - within);
			piece(*drives[c % drives.size()], c / drives.size() * stripe + within, n, done);
			done += n;
		}
	} catch (...) {
		// The pieces already started on other drives write to the caller's frame, as in Drive's Issue
		for (auto &d : drives) {
			try {
				d->Finish();
			} catch (Error &) {
			}
		}
		throw;
	}
}

void Member::BeginRead(void *buf, uint64_t nbytes, uint64_t offset, Completion *c) {
	if (!stripe) {
		return drives[0]->BeginRead(buf, nbytes, offset, c);
	}
	auto out = static_cast<uint8_t *>(buf);
	Split(offset, nbytes, [&](Drive &d, uint64_t at, uint64_t n, uint64_t done) { d.BeginRead(out + done, n, at, c); });
}

void Member::BeginWrite(const void *buf, uint64_t nbytes, uint64_t offset, Completion *c) {
	if (!stripe) {
		return drives[0]->BeginWrite(buf, nbytes, offset, c);
	}
	auto in = static_cast<const uint8_t *>(buf);
	Split(offset, nbytes, [&](Drive &d, uint64_t at, uint64_t n, uint64_t done) { d.BeginWrite(in + done, n, at, c); });
}

void Member::BeginReadDirect(void *dma, uint64_t nbytes, uint64_t offset, Completion *c) {
	if (!stripe) {
		return drives[0]->BeginReadDirect(dma, nbytes, offset, c);
	}
	auto out = static_cast<uint8_t *>(dma);
	Split(offset, nbytes,
	      [&](Drive &d, uint64_t at, uint64_t n, uint64_t done) { d.BeginReadDirect(out + done, n, at, c); });
}

void Member::BeginWriteDirect(const void *dma, uint64_t nbytes, uint64_t offset, Completion *c) {
	if (!stripe) {
		return drives[0]->BeginWriteDirect(dma, nbytes, offset, c);
	}
	auto in = static_cast<const uint8_t *>(dma);
	Split(offset, nbytes,
	      [&](Drive &d, uint64_t at, uint64_t n, uint64_t done) { d.BeginWriteDirect(in + done, n, at, c); });
}

void Member::BeginFlush(Completion *c) {
	for (auto &d : drives) {
		d->BeginFlush(c);
	}
}

void *Member::AllocDma(uint64_t nbytes) {
	return drives[0]->AllocDma(nbytes);
}

void Member::FreeDma(void *p) {
	drives[0]->FreeDma(p);
}

void Member::RegisterDmabuf(int fd, void *vaddr) {
	drives[0]->RegisterDmabuf(fd, vaddr);
}

void Member::UnregisterDmabuf(void *vaddr) {
	drives[0]->UnregisterDmabuf(vaddr);
}

void Member::Poke() {
	for (auto &d : drives) {
		d->Poke();
	}
}

void Member::Finish(int cls) {
	// Every drive is waited out before a failure is passed on, since commands left in flight write to the caller's
	// buffers
	std::exception_ptr failed;
	for (auto &d : drives) {
		try {
			d->Finish(cls);
		} catch (...) {
			if (!failed) {
				failed = std::current_exception();
			}
		}
	}
	if (failed) {
		std::rethrow_exception(failed);
	}
}

void Member::Zero(uint64_t offset, uint64_t nbytes) {
	if (!stripe) {
		return drives[0]->Zero(offset, nbytes);
	}
	// A contiguous range of the set is a contiguous range on each drive
	std::vector<uint64_t> lo(drives.size(), UINT64_MAX), hi(drives.size(), 0);
	Split(offset, nbytes, [&](Drive &d, uint64_t at, uint64_t n, uint64_t) {
		for (size_t i = 0; i < drives.size(); i++) {
			if (drives[i].get() == &d) {
				lo[i] = std::min(lo[i], at);
				hi[i] = std::max(hi[i], at + n);
			}
		}
	});
	for (size_t i = 0; i < drives.size(); i++) {
		if (hi[i]) {
			drives[i]->Zero(lo[i], hi[i] - lo[i]);
		}
	}
}

void Member::Read(void *buf, uint64_t nbytes, uint64_t offset) {
	BeginRead(buf, nbytes, offset);
	Finish(WaitClass(nbytes));
}

void Member::Write(const void *buf, uint64_t nbytes, uint64_t offset) {
	BeginWrite(buf, nbytes, offset);
	Finish(WaitClass(nbytes));
}

void Member::Sync() {
	BeginFlush();
	Finish();
}

static void CheckNames(const std::string &name, const std::vector<std::string> &members) {
	if (name.empty() || name.size() >= MAX_POOL_NAME) {
		Fail(EINVAL, "xnvme fs: a pool name is 1 to %zu characters", MAX_POOL_NAME - 1);
	}
	for (char c : name) {
		if (!isalnum((unsigned char)c) && c != '_' && c != '-' && c != '.') {
			Fail(EINVAL, "xnvme fs: pool name '%s' may hold only letters, digits, '_', '-' and '.'", name.c_str());
		}
	}
	if (members.empty()) {
		Fail(EINVAL, "xnvme fs: no pool members");
	}
	if (members.size() > MAX_MEMBERS) {
		Fail(EINVAL, "xnvme fs: a pool has at most %zu members", MAX_MEMBERS);
	}
	for (size_t i = 0; i < members.size(); i++) {
		if (members[i].empty() || members[i].size() >= MAX_MEMBER_PATH) {
			Fail(EINVAL, "xnvme fs: a member is named by 1 to %zu characters", MAX_MEMBER_PATH - 1);
		}
		for (size_t j = 0; j < i; j++) {
			if (members[i] == members[j]) {
				Fail(EINVAL, "xnvme fs: member '%s' is listed twice", members[i].c_str());
			}
		}
	}
}

static bool HoldsSuperblock(Member &member) {
	uint8_t block[SUPERBLOCK_SIZE];
	if (member.Size() < SUPERBLOCK_SIZE) {
		return false;
	}
	member.Read(block, SUPERBLOCK_SIZE, 0);
	return memcmp(block, MAGIC, sizeof(MAGIC)) == 0;
}

std::unique_ptr<Pool> Pool::Format(const std::string &name, const std::vector<std::string> &members,
                                   const Access &access, uint64_t extent_size, bool overwrite) {
	ValidateAccess(access);
	CheckNames(name, members);
	if (extent_size < MIN_EXTENT_SIZE || (extent_size & (extent_size - 1))) {
		Fail(EINVAL, "xnvme fs: the extent size is a power of two of at least %llu bytes",
		     (unsigned long long)MIN_EXTENT_SIZE);
	}
	auto pool = std::unique_ptr<Pool>(new Pool());
	for (auto &m : members) {
		pool->members.push_back(std::unique_ptr<Member>(new Member(m, access)));
	}
	uint64_t smallest = pool->members[0]->Size();
	for (auto &m : pool->members) {
		smallest = std::min(smallest, m->Size());
		if (!overwrite && HoldsSuperblock(*m)) {
			Fail(EEXIST, "xnvme fs: member '%s' already holds a pool superblock; pass overwrite", m->Path().c_str());
		}
	}

	// Each slot holds every extent of the pool once, plus room for about ten thousand names.
	auto &sb = pool->superblock;
	uint64_t total_extents = smallest / extent_size * members.size();
	sb.extent_size = extent_size;
	sb.slot_size = AlignUp((1ULL << 20) + total_extents * 8, SUPERBLOCK_SIZE);
	sb.data_offset = AlignUp(SUPERBLOCK_SIZE + 2 * sb.slot_size, DATA_ALIGN);
	if (smallest < sb.data_offset + extent_size) {
		Fail(EINVAL, "xnvme fs: a member of %llu bytes holds no extent of %llu bytes after its metadata area",
		     (unsigned long long)smallest, (unsigned long long)extent_size);
	}

	std::random_device rd;
	sb.pool_name = name;
	sb.members = members;
	for (auto &b : sb.pool_uuid) {
		b = uint8_t(rd());
	}

	// Clear both slots first, so a table left by an earlier pool on the same member cannot be read back.
	std::vector<uint8_t> zero(SUPERBLOCK_SIZE, 0);
	pool->members[0]->Write(zero.data(), zero.size(), SUPERBLOCK_SIZE);
	pool->members[0]->Write(zero.data(), zero.size(), SUPERBLOCK_SIZE + sb.slot_size);
	Table empty;
	empty.generation = 1;
	pool->WriteTable(empty);

	for (size_t i = 0; i < pool->members.size(); i++) {
		sb.member_index = uint32_t(i);
		uint8_t block[SUPERBLOCK_SIZE];
		sb.Encode(block);
		pool->members[i]->Write(block, SUPERBLOCK_SIZE, 0);
		pool->members[i]->Sync();
	}
	sb.member_index = 0;
	return pool;
}

Superblock Pool::Peek(const std::string &member, const Access &access) {
	ValidateAccess(access);
	Member m(member, access);
	std::string where = "member '" + member + "'";
	if (m.Size() < SUPERBLOCK_SIZE) {
		Fail(EINVAL, "xnvme fs: %s is smaller than a superblock", where.c_str());
	}
	uint8_t block[SUPERBLOCK_SIZE];
	m.Read(block, SUPERBLOCK_SIZE, 0);
	return Superblock::Decode(block, where);
}

std::unique_ptr<Pool> Pool::Open(const std::string &name, const std::vector<std::string> &members,
                                 const Access &access) {
	ValidateAccess(access);
	if (members.empty()) {
		Fail(EINVAL, "xnvme fs: no pool members");
	}
	auto pool = std::unique_ptr<Pool>(new Pool());
	for (size_t i = 0; i < members.size(); i++) {
		auto member = std::unique_ptr<Member>(new Member(members[i], access));
		std::string where = "member '" + members[i] + "'";
		if (member->Size() < SUPERBLOCK_SIZE) {
			Fail(EINVAL, "xnvme fs: %s is smaller than a superblock", where.c_str());
		}
		uint8_t block[SUPERBLOCK_SIZE];
		member->Read(block, SUPERBLOCK_SIZE, 0);
		auto sb = Superblock::Decode(block, where);

		if (sb.pool_name != name) {
			Fail(EINVAL, "xnvme fs: %s belongs to pool '%s', not '%s'", where.c_str(), sb.pool_name.c_str(),
			     name.c_str());
		}
		if (sb.members != members) {
			std::string have, want;
			for (auto &m : sb.members) {
				have += (have.empty() ? "" : ", ") + m;
			}
			for (auto &m : members) {
				want += (want.empty() ? "" : ", ") + m;
			}
			Fail(EINVAL, "xnvme fs: %s lists members [%s], not [%s]", where.c_str(), have.c_str(), want.c_str());
		}
		if (sb.member_index != i) {
			Fail(EINVAL, "xnvme fs: %s is member %u of the pool, listed as member %zu", where.c_str(), sb.member_index,
			     i);
		}
		if (i > 0) {
			auto &first = pool->superblock;
			if (memcmp(sb.pool_uuid, first.pool_uuid, sizeof(sb.pool_uuid)) != 0) {
				Fail(EINVAL, "xnvme fs: %s belongs to another instance of pool '%s'", where.c_str(), name.c_str());
			}
			if (sb.extent_size != first.extent_size || sb.slot_size != first.slot_size ||
			    sb.data_offset != first.data_offset) {
				Fail(EINVAL, "xnvme fs: %s has another geometry than member 0", where.c_str());
			}
		} else {
			pool->superblock = sb;
		}
		if (member->Size() < sb.data_offset) {
			Fail(EINVAL, "xnvme fs: %s is smaller than its metadata area", where.c_str());
		}
		pool->members.push_back(std::move(member));
	}
	pool->superblock.member_index = 0;
	return pool;
}

uint64_t Pool::ExtentsOn(size_t member) const {
	return (members[member]->Size() - superblock.data_offset) / superblock.extent_size;
}

std::vector<MemberInfo> Pool::Members() const {
	std::vector<MemberInfo> info;
	for (size_t i = 0; i < members.size(); i++) {
		info.push_back({members[i]->Path(), members[i]->Size(), ExtentsOn(i)});
	}
	return info;
}

Table Pool::ReadTable() {
	Table best;
	bool found = false;
	for (int slot = 0; slot < 2; slot++) {
		std::vector<uint8_t> buf(superblock.slot_size);
		members[0]->Read(buf.data(), buf.size(), SUPERBLOCK_SIZE + slot * superblock.slot_size);
		Table t;
		bool valid = false;
		try {
			valid = DecodeTable(buf, superblock, t);
		} catch (Error &) {
			valid = false;
		}
		if (valid && (!found || t.generation > best.generation)) {
			best = std::move(t);
			found = true;
		}
	}
	if (!found) {
		Fail(EINVAL, "xnvme fs: pool '%s' holds no valid extent map table", superblock.pool_name.c_str());
	}
	return best;
}

void Pool::WriteTable(const Table &table) {
	auto buf = EncodeTable(table, superblock);
	uint64_t slot = table.generation % 2;
	members[0]->Write(buf.data(), buf.size(), SUPERBLOCK_SIZE + slot * superblock.slot_size);
	members[0]->Sync();
}

} // namespace xnvme_fs
