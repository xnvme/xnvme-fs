// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
// SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
//
// SPDX-License-Identifier: BSD-3-Clause
//
// Writes bytes at a byte offset of a controller through a member, for tests that damage a superblock or a table on
// purpose, reads bytes to stdout, for tests that check a range was left alone, or writes bytes from stdin; what dd did
// when members were files. With --threads, as many threads read the range at once, each with its queues held until
// all have read, and the bytes are written once if every thread read the same.
//
//   xnvme-test-fs-poke 0000:01:00.0 HOMI_ID OFFSET HEX       e.g. xnvme-test-fs-poke 0000:01:00.0 1 0 4e4e4e4e4e4e4e4e
//   xnvme-test-fs-poke 0000:01:00.0 HOMI_ID OFFSET --read NBYTES [--threads N]
//   xnvme-test-fs-poke 0000:01:00.0 HOMI_ID OFFSET --write NBYTES

#include "error.hpp"
#include "layout.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace xnvme_fs;

//! Holds each thread until 'count' have arrived.
class Barrier {
public:
	explicit Barrier(size_t count_p) : count(count_p) {
	}
	void Wait() {
		std::unique_lock<std::mutex> guard(lock);
		auto round = rounds;
		if (++arrived == count) {
			arrived = 0;
			rounds++;
			cv.notify_all();
			return;
		}
		cv.wait(guard, [&] { return rounds != round; });
	}

private:
	std::mutex lock;
	std::condition_variable cv;
	size_t count;
	size_t arrived = 0;
	size_t rounds = 0;
};

static int ReadThreads(Member &m, uint64_t offset, uint64_t nbytes, size_t nthreads) {
	std::vector<std::vector<uint8_t>> bufs(nthreads, std::vector<uint8_t>(nbytes));
	std::vector<std::string> errors(nthreads);
	Barrier barrier(nthreads);
	std::vector<std::thread> threads;
	for (size_t t = 0; t < nthreads; t++) {
		threads.emplace_back([&, t] {
			// Every thread makes its queues, on the drives holding either end of the range, before any reads the
			// range, so they all hold their first slots
			std::vector<uint8_t> first(4096);
			try {
				m.Read(first.data(), first.size(), offset);
				m.Read(first.data(), first.size(), offset + nbytes - first.size());
			} catch (Error &err) {
				errors[t] = err.what();
			}
			barrier.Wait();
			try {
				if (errors[t].empty()) {
					m.Read(bufs[t].data(), nbytes, offset);
				}
			} catch (Error &err) {
				errors[t] = err.what();
			}
			barrier.Wait();
		});
	}
	for (auto &thread : threads) {
		thread.join();
	}
	for (size_t t = 0; t < nthreads; t++) {
		if (!errors[t].empty()) {
			fprintf(stderr, "xnvme-test-fs-poke: thread %zu: %s\n", t, errors[t].c_str());
			return 1;
		}
		if (bufs[t] != bufs[0]) {
			fprintf(stderr, "xnvme-test-fs-poke: thread %zu read other bytes than thread 0\n", t);
			return 1;
		}
	}
	fwrite(bufs[0].data(), 1, nbytes, stdout);
	return 0;
}

int main(int argc, char **argv) {
	bool threaded = argc == 8 && std::string(argv[6]) == "--threads";
	if ((argc == 6 || threaded) && std::string(argv[4]) == "--read") {
		try {
			Access access;
			access.homi_id = uint32_t(strtoul(argv[2], nullptr, 10));
			Member m(argv[1], access);
			uint64_t offset = strtoull(argv[3], nullptr, 10);
			uint64_t left = strtoull(argv[5], nullptr, 10);
			if (threaded) {
				return ReadThreads(m, offset, left, strtoul(argv[7], nullptr, 10));
			}
			std::vector<uint8_t> buf(1 << 20);
			while (left) {
				uint64_t n = std::min<uint64_t>(left, buf.size());
				m.Read(buf.data(), n, offset);
				fwrite(buf.data(), 1, n, stdout);
				offset += n;
				left -= n;
			}
		} catch (Error &err) {
			fprintf(stderr, "xnvme-test-fs-poke: %s\n", err.what());
			return 1;
		}
		return 0;
	}
	if (argc == 6 && std::string(argv[4]) == "--write") {
		try {
			Access access;
			access.homi_id = uint32_t(strtoul(argv[2], nullptr, 10));
			Member m(argv[1], access);
			uint64_t offset = strtoull(argv[3], nullptr, 10);
			uint64_t left = strtoull(argv[5], nullptr, 10);
			std::vector<uint8_t> buf(1 << 20);
			while (left) {
				uint64_t n = std::min<uint64_t>(left, buf.size());
				if (fread(buf.data(), 1, n, stdin) != n) {
					fprintf(stderr, "xnvme-test-fs-poke: stdin ended before %s bytes\n", argv[5]);
					return 1;
				}
				m.Write(buf.data(), n, offset);
				offset += n;
				left -= n;
			}
			m.Sync();
		} catch (Error &err) {
			fprintf(stderr, "xnvme-test-fs-poke: %s\n", err.what());
			return 1;
		}
		return 0;
	}
	if (argc != 5) {
		fprintf(stderr,
		        "usage: xnvme-test-fs-poke MEMBER HOMI_ID OFFSET HEX | --read NBYTES [--threads N] | --write NBYTES\n");
		return 64;
	}
	std::string hex = argv[4];
	if (hex.empty() || hex.size() % 2) {
		fprintf(stderr, "xnvme-test-fs-poke: HEX is an even number of hex digits\n");
		return 64;
	}
	std::vector<uint8_t> bytes;
	for (size_t i = 0; i < hex.size(); i += 2) {
		bytes.push_back(uint8_t(strtoul(hex.substr(i, 2).c_str(), nullptr, 16)));
	}
	try {
		Access access;
		access.homi_id = uint32_t(strtoul(argv[2], nullptr, 10));
		Member m(argv[1], access);
		m.Write(bytes.data(), bytes.size(), strtoull(argv[3], nullptr, 10));
		m.Sync();
	} catch (Error &err) {
		fprintf(stderr, "xnvme-test-fs-poke: %s\n", err.what());
		return 1;
	}
	return 0;
}
