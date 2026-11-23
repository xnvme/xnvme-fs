// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
// SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
//
// SPDX-License-Identifier: BSD-3-Clause
//
// A store of numbered records on a pool 'xnvme fs serve' serves, written the way a database writes: each record is
// appended to a log and synced, then acknowledged on stdout as "ack K"; every fiftieth record is a checkpoint, which
// writes the log's records to the data file and syncs it, writes the count to a new file renamed over the old one, and
// truncates the log. A test kills this process, and the server, at any point, and then asks what the pool holds:
// every record from 1 to the last, each with its own bytes, and no log or count cut short.
//
//   xnvme-test-fs-crash POOL write COUNT    brings the store up to date, then appends COUNT records
//   xnvme-test-fs-crash POOL check          brings the store up to date and prints the number of the last record
//
// Run beside the server, with XNVME_FS_SOCKET_DIR and XNVME_FS_HOMI_ID set.

#include "error.hpp"
#include "session.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace xnvme_fs;

//! Not a multiple of a block, so a record's end and the next one's start share a block
static constexpr uint64_t RECORD = 1000;
static constexpr uint64_t CHECKPOINT_EVERY = 50;
static const char *DATA = "crash.db";
static const char *LOG = "crash.wal";
static const char *COUNT = "crash.count";
static const char *COUNT_NEW = "crash.count.new";

static void PutNumber(uint8_t *at, uint64_t k) {
	for (int i = 0; i < 8; i++) {
		at[i] = uint8_t(k >> (8 * i));
	}
}

static uint64_t GetNumber(const uint8_t *at) {
	uint64_t k = 0;
	for (int i = 0; i < 8; i++) {
		k |= uint64_t(at[i]) << (8 * i);
	}
	return k;
}

static void Fill(uint8_t *record, uint64_t k) {
	PutNumber(record, k);
	for (uint64_t i = 8; i < RECORD; i++) {
		record[i] = uint8_t(k + i);
	}
}

static bool Holds(const uint8_t *record, uint64_t k) {
	std::vector<uint8_t> want(RECORD);
	Fill(want.data(), k);
	return memcmp(record, want.data(), RECORD) == 0;
}

static std::vector<uint8_t> ReadAll(File &file, uint64_t nbytes) {
	std::vector<uint8_t> bytes(nbytes);
	uint64_t at = 0;
	while (at < nbytes) {
		auto n = file.Read(bytes.data() + at, std::min<uint64_t>(nbytes - at, 1 << 20), at);
		if (n == 0) {
			Fail(EIO, "'%s' ends at %llu of its %llu bytes", file.Name().c_str(), (unsigned long long)at,
			     (unsigned long long)nbytes);
		}
		at += n;
	}
	return bytes;
}

struct Store {
	std::shared_ptr<Session> session;
	std::shared_ptr<File> data;
	std::shared_ptr<File> log;
	//! The data file holds the records 1 to 'checkpointed'; 'pending' holds those after, to 'last', as the log does.
	uint64_t checkpointed = 0;
	uint64_t last = 0;
	std::vector<uint8_t> pending;

	//! Reads what the pool holds, and fails unless that is every record from 1 to the last.
	void Load() {
		if (session->client.StatName(COUNT).kind) {
			auto count = session->Open(COUNT, 0);
			if (count->Size() != 8) {
				Fail(EIO, "'%s' holds %llu bytes, not a count", COUNT, (unsigned long long)count->Size());
			}
			checkpointed = GetNumber(ReadAll(*count, 8).data());
		}
		data = session->Open(DATA, OPEN_CREATE);
		if (data->Size() < checkpointed * RECORD) {
			Fail(EIO, "'%s' holds %llu bytes, fewer than the %llu records counted", DATA,
			     (unsigned long long)data->Size(), (unsigned long long)checkpointed);
		}
		auto records = ReadAll(*data, checkpointed * RECORD);
		for (uint64_t k = 1; k <= checkpointed; k++) {
			if (!Holds(records.data() + (k - 1) * RECORD, k)) {
				Fail(EIO, "'%s' does not hold record %llu where it lies", DATA, (unsigned long long)k);
			}
		}

		// A checkpoint killed after its rename leaves the records it wrote in the log
		log = session->Open(LOG, OPEN_CREATE);
		auto nbytes = log->Size();
		if (nbytes % RECORD) {
			Fail(EIO, "'%s' holds %llu bytes, a record cut short", LOG, (unsigned long long)nbytes);
		}
		records = ReadAll(*log, nbytes);
		last = checkpointed;
		for (uint64_t i = 0; i < nbytes / RECORD; i++) {
			auto record = records.data() + i * RECORD;
			auto k = GetNumber(record);
			if (!Holds(record, k)) {
				Fail(EIO, "'%s' holds other bytes than record %llu at %llu", LOG, (unsigned long long)k,
				     (unsigned long long)(i * RECORD));
			}
			if (i == 0 ? k > checkpointed + 1 : k != GetNumber(record - RECORD) + 1) {
				Fail(EIO, "'%s' holds record %llu where %llu records precede it, a gap", LOG, (unsigned long long)k,
				     (unsigned long long)(i ? GetNumber(record - RECORD) : checkpointed));
			}
			if (k > checkpointed) {
				pending.insert(pending.end(), record, record + RECORD);
				last = k;
			}
		}
		if (nbytes && GetNumber(records.data() + nbytes - RECORD) < checkpointed) {
			Fail(EIO, "'%s' ends before record %llu, the last one counted", LOG, (unsigned long long)checkpointed);
		}
	}

	void Append(uint64_t k) {
		std::vector<uint8_t> record(RECORD);
		Fill(record.data(), k);
		log->Write(record.data(), RECORD, log->Size());
		log->Sync();
		pending.insert(pending.end(), record.begin(), record.end());
		last = k;
	}

	void Checkpoint() {
		if (!pending.empty()) {
			data->Write(pending.data(), pending.size(), checkpointed * RECORD);
			data->Sync();
			uint8_t count[8];
			PutNumber(count, last);
			{
				auto next = session->Open(COUNT_NEW, OPEN_CREATE | OPEN_TRUNCATE);
				next->Write(count, sizeof(count), 0);
				next->Sync();
			}
			session->client.Rename(COUNT_NEW, COUNT);
			session->Renamed(COUNT_NEW, COUNT);
			checkpointed = last;
			pending.clear();
		}
		if (log->Size()) {
			log->Truncate(0);
			log->Sync();
		}
	}
};

int main(int argc, char **argv) {
	const char *dir = getenv("XNVME_FS_SOCKET_DIR");
	const char *homi = getenv("XNVME_FS_HOMI_ID");
	bool write = argc == 4 && std::string(argv[2]) == "write";
	bool check = argc == 3 && std::string(argv[2]) == "check";
	if (!(write || check) || !dir) {
		fprintf(stderr, "usage: XNVME_FS_SOCKET_DIR=DIR [XNVME_FS_HOMI_ID=N] xnvme-test-fs-crash POOL write COUNT | "
		                "check\n");
		return 64;
	}
	try {
		Access access;
		access.homi_id = homi ? uint32_t(strtoul(homi, nullptr, 10)) : 0;
		Store store;
		store.session = std::make_shared<Session>(argv[1], std::string(dir) + "/" + argv[1] + ".sock", access);
		store.Load();
		store.Checkpoint();
		if (check) {
			printf("%llu\n", (unsigned long long)store.last);
			return 0;
		}
		auto end = store.last + strtoull(argv[3], nullptr, 10);
		for (auto k = store.last + 1; k <= end; k++) {
			store.Append(k);
			printf("ack %llu\n", (unsigned long long)k);
			fflush(stdout);
			if (k % CHECKPOINT_EVERY == 0) {
				store.Checkpoint();
			}
		}
	} catch (Error &err) {
		fprintf(stderr, "xnvme-test-fs-crash: %s\n", err.what());
		return 1;
	}
	return 0;
}
