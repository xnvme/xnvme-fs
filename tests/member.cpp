// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
// SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
//
// SPDX-License-Identifier: BSD-3-Clause
//
// A member's byte-addressed reads and writes against an in-memory copy: unaligned ranges, ranges larger than one
// command, concurrent writes of neighbouring records that share blocks, writes of a thread that exits before they
// complete, and a read past the namespace's end failing with the device's status, or through a block device having
// moved no bytes. The controller's first 64 MiB are overwritten. Under a kernel backend (XNVME_FS_BACKEND) the member
// is a namespace's device and HOMI_ID is ignored.
//
//   xnvme-test-fs-member 0000:01:00.0 HOMI_ID

#include "error.hpp"
#include "layout.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

using namespace xnvme_fs;

static constexpr uint64_t SIZE = 64ULL << 20;

static int Check(const std::vector<uint8_t> &want, Member &m, const char *what) {
	std::vector<uint8_t> got(want.size());
	m.Read(got.data(), got.size(), 0);
	for (size_t i = 0; i < want.size(); i++) {
		if (got[i] != want[i]) {
			fprintf(stderr, "xnvme-test-fs-member: %s: byte %zu is %u, want %u\n", what, i, got[i], want[i]);
			return 1;
		}
	}
	return 0;
}

int main(int argc, char **argv) {
	if (argc != 3) {
		fprintf(stderr, "usage: xnvme-test-fs-member BDF HOMI_ID\n");
		return 64;
	}
	Access access;
	access.homi_id = uint32_t(strtoul(argv[2], nullptr, 10));

	try {
		Member m(argv[1], access);
		// A drive holds whatever it held; the copy starts from that, not from zeros.
		std::vector<uint8_t> want(SIZE);
		m.Read(want.data(), SIZE, 0);
		std::mt19937_64 rng(42);

		// Random ranges, most not block-aligned, up to three times a command's limit.
		for (int op = 0; op < 400; op++) {
			uint64_t len = 1 + rng() % (3ULL << 20);
			uint64_t off = rng() % (SIZE - len);
			std::vector<uint8_t> data(len);
			for (auto &b : data) {
				b = uint8_t(rng());
			}
			m.Write(data.data(), len, off);
			memcpy(want.data() + off, data.data(), len);
			if (op % 20 == 0) {
				uint64_t rlen = 1 + rng() % (3ULL << 20);
				uint64_t roff = rng() % (SIZE - rlen);
				std::vector<uint8_t> got(rlen);
				m.Read(got.data(), rlen, roff);
				for (uint64_t i = 0; i < rlen; i++) {
					if (got[i] != want[roff + i]) {
						fprintf(stderr,
						        "xnvme-test-fs-member: read of %llu bytes at %llu differs first at byte %llu: %u, want %u\n",
						        (unsigned long long)rlen, (unsigned long long)roff, (unsigned long long)(roff + i),
						        got[i], want[roff + i]);
						return 1;
					}
				}
			}
		}
		m.Sync();
		if (Check(want, m, "after random writes")) {
			return 1;
		}

		// Four threads write interleaved 100-byte records, so most blocks are shared by records of several.
		constexpr int THREADS = 4;
		constexpr uint64_t RECORD = 100;
		constexpr uint64_t RECORDS = 40000;
		std::vector<std::thread> threads;
		for (int t = 0; t < THREADS; t++) {
			threads.emplace_back([&m, t]() {
				uint8_t rec[RECORD];
				for (uint64_t i = t; i < RECORDS; i += THREADS) {
					memset(rec, int(i % 251), sizeof(rec));
					m.Write(rec, RECORD, i * RECORD);
				}
			});
		}
		for (auto &th : threads) {
			th.join();
		}
		for (uint64_t i = 0; i < RECORDS; i++) {
			memset(want.data() + i * RECORD, int(i % 251), RECORD);
		}
		if (Check(want, m, "after concurrent records")) {
			return 1;
		}

		// A thread that exits with writes in flight gives its queue back only once they have landed
		std::vector<uint8_t> tail(32 * m.CommandBytes());
		for (size_t i = 0; i < tail.size(); i++) {
			tail[i] = uint8_t(i * 7 + 3);
		}
		Completion exited;
		std::thread([&]() { m.BeginWrite(tail.data(), tail.size(), 0, &exited); }).join();
		if (exited.outstanding || exited.status) {
			fprintf(stderr, "xnvme-test-fs-member: %llu writes of an exited thread outstanding, status %d\n",
			        (unsigned long long)exited.outstanding, exited.status);
			return 1;
		}
		memcpy(want.data(), tail.data(), tail.size());
		if (Check(want, m, "after a thread exited with writes in flight")) {
			return 1;
		}

		std::vector<uint8_t> block(m.BlockSize());
		Completion c;
		m.BeginRead(block.data(), block.size(), m.Size(), &c);
		while (c.outstanding) {
			m.Poke();
		}
		// The controller refuses it with an NVMe status; a block device moves no bytes
		if (c.status != EIO ||
		    (c.why.find("NVMe status") == std::string::npos && c.why.find("transferred 0 of") == std::string::npos)) {
			fprintf(stderr, "xnvme-test-fs-member: a read past the end gave status %d, '%s'\n", c.status, c.why.c_str());
			return 1;
		}
	} catch (Error &err) {
		fprintf(stderr, "xnvme-test-fs-member: %s\n", err.what());
		return 1;
	}
	printf("xnvme-test-fs-member: ok\n");
	return 0;
}
