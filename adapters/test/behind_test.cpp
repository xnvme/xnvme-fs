// SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
//
// SPDX-License-Identifier: MIT
//
// The DuckDB adapter's writes behind, on a pool 'xnvme fs serve' serves: a range written again by another thread while
// the first write is unreaped, read through the overlay and from the drive once both are reaped, the same with
// the first write's thread exiting before it reaps, and a thread exiting whose queues go before its unreaped writes.
// Run beside the server, as with-xnvme-fs.sh does, with XNVME_FS_SOCKET_DIR and XNVME_FS_HOMI_ID set.
//
//   behind_test POOL

#include "xnvme_fs.hpp"
#include "write_behind.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

using namespace duckdb::xnvme_fs;

static constexpr uint64_t SLOT = 256 * 1024;
static int failures;

static void Expect(::xnvme::fs::File &file, bool overlay, uint8_t want, const char *what) {
	std::vector<uint8_t> got(SLOT);
	Overlay o(file, SLOT, 0);
	file.Read(got.data(), SLOT, 0);
	if (overlay) {
		o.Apply(got.data());
	}
	for (uint64_t i = 0; i < SLOT; i++) {
		if (got[i] != want) {
			fprintf(stderr, "behind_test: %s: byte %llu is '%c', want '%c'\n", what, (unsigned long long)i, got[i],
			        want);
			failures++;
			return;
		}
	}
}

static void Fill(const std::shared_ptr<::xnvme::fs::File> &file, uint8_t c) {
	std::vector<uint8_t> bytes(SLOT, c);
	WriteBehind(file, bytes.data(), SLOT, 0);
}

int main(int argc, char **argv) {
	const char *dir = getenv("XNVME_FS_SOCKET_DIR");
	const char *homi = getenv("XNVME_FS_HOMI_ID");
	if (argc != 2 || !dir) {
		fprintf(stderr, "usage: XNVME_FS_SOCKET_DIR=DIR [XNVME_FS_HOMI_ID=N] behind_test POOL\n");
		return 64;
	}
	try {
		auto session =
		    std::make_shared<::xnvme::fs::Session>(argv[1], dir, "", homi ? uint32_t(strtoul(homi, nullptr, 10)) : 0);
		auto file = session->Open("behind/data", XNVME_FS_O_CREATE | XNVME_FS_O_TRUNC);

		Fill(file, 'a');
		std::thread([&]() {
			Fill(file, 'b');
			Reap(true);
		}).join();
		Expect(*file, true, 'b', "rewritten by another thread, the first write unreaped");
		Reap(true);
		Expect(*file, false, 'b', "rewritten by another thread, both reaped");

		// The thread exits only once its write is written over, so it leaves a write to rewrite to the next to reap
		std::atomic<int> step {0};
		std::thread exiting([&]() {
			Fill(file, 'c');
			step = 1;
			while (step != 2) {
			}
		});
		while (step != 1) {
		}
		Fill(file, 'd');
		step = 2;
		exiting.join();
		Reap(true);
		Expect(*file, true, 'd', "rewritten after the first write's thread exited");
		Reap(true);
		Expect(*file, false, 'd', "rewritten after the first write's thread exited, both reaped");
		if (InFlightPast(*file, 0)) {
			fprintf(stderr, "behind_test: a write is in flight after every thread reaped\n");
			failures++;
		}

		// Reaping first makes the thread's list of writes older than its queues, so at its exit the queues go first
		// and the list's last reap finds them gone
		std::thread([&]() {
			Reap(false);
			Fill(file, 'e');
		}).join();
		Reap(true);
		Expect(*file, false, 'e', "written by a thread whose queues went before its writes were reaped");
	} catch (std::system_error &err) {
		fprintf(stderr, "behind_test: %s\n", err.what());
		return 1;
	}
	if (failures) {
		return 1;
	}
	printf("behind_test: ok\n");
	return 0;
}
