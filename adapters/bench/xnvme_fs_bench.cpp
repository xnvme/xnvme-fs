// SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
//
// SPDX-License-Identifier: MIT
//
// xnvme-fs-bench: a workload on one file of a pool, through libxnvme-fs, as an engine would drive it.
//
//   xnvme-fs-bench --pool NAME [--socket-dir DIR] [--homi-id N] [--file NAME] [--size BYTES]
//                  [--workload randread|seqread|randwrite|seqwrite|walsync] [--bs BYTES] [--threads N]
//                  [--api sync|async] [--qd N] [--seconds S | --ops N] [--verify] [--json FILE]
//
// The defaults mirror the engines: 4 KiB random reads are RocksDB point lookups, 256 KiB sequential reads DuckDB
// scans and compaction input, 256 KiB writes spills and SST files, small appends with a sync each a write-ahead log.
// With --verify every 4 KiB sector carries its own file offset; reads check it, and after a writing workload the
// whole file is read back and checked, so a misplaced, torn or stale sector fails the run.

#include "xnvme_fs.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <vector>

using namespace xnvme::fs;
using Clock = std::chrono::steady_clock;

static constexpr uint64_t SECTOR = 4096;

struct Args {
	std::string pool;
	std::string socket_dir;
	uint32_t homi_id = 0;
	std::string file = "bench.dat";
	uint64_t size = 1ULL << 30;
	std::string workload = "randread";
	uint64_t bs = 0;
	unsigned threads = 1;
	std::string api = "sync";
	unsigned qd = 1;
	double seconds = 10;
	uint64_t ops = 0;
	bool verify = false;
	std::string json;
};

[[noreturn]] static void Usage() {
	fprintf(stderr, "usage: xnvme-fs-bench --pool NAME [--socket-dir DIR] [--homi-id N] [--file NAME] [--size BYTES]\n"
	                "        [--workload randread|seqread|randwrite|seqwrite|walsync] [--bs BYTES] [--threads N]\n"
	                "        [--api sync|async] [--qd N] [--seconds S | --ops N] [--verify] [--json FILE]\n");
	exit(64);
}

static uint64_t ParseSize(const std::string &s) {
	char *end = nullptr;
	unsigned long long v = strtoull(s.c_str(), &end, 10);
	switch (*end) {
	case 'G':
		v <<= 10;
		// fall through
	case 'M':
		v <<= 10;
		// fall through
	case 'K':
		v <<= 10;
		break;
	default:
		break;
	}
	return v;
}

static Args Parse(int argc, char **argv) {
	Args a;
	auto env = getenv("XNVME_FS_SOCKET_DIR");
	a.socket_dir = env && *env ? env : "/run/xnvme-fs";
	env = getenv("XNVME_FS_HOMI_ID");
	a.homi_id = env && *env ? uint32_t(strtoul(env, nullptr, 10)) : 0;
	for (int i = 1; i < argc; i++) {
		std::string opt = argv[i];
		if (opt == "--verify") {
			a.verify = true;
			continue;
		}
		if (i + 1 >= argc) {
			Usage();
		}
		std::string val = argv[++i];
		if (opt == "--pool") {
			a.pool = val;
		} else if (opt == "--socket-dir") {
			a.socket_dir = val;
		} else if (opt == "--homi-id") {
			a.homi_id = uint32_t(strtoul(val.c_str(), nullptr, 10));
		} else if (opt == "--file") {
			a.file = val;
		} else if (opt == "--size") {
			a.size = ParseSize(val);
		} else if (opt == "--workload") {
			a.workload = val;
		} else if (opt == "--bs") {
			a.bs = ParseSize(val);
		} else if (opt == "--threads") {
			a.threads = unsigned(strtoul(val.c_str(), nullptr, 10));
		} else if (opt == "--api") {
			a.api = val;
		} else if (opt == "--qd") {
			a.qd = unsigned(strtoul(val.c_str(), nullptr, 10));
		} else if (opt == "--seconds") {
			a.seconds = strtod(val.c_str(), nullptr);
		} else if (opt == "--ops") {
			a.ops = strtoull(val.c_str(), nullptr, 10);
		} else if (opt == "--json") {
			a.json = val;
		} else {
			Usage();
		}
	}
	bool known = a.workload == "randread" || a.workload == "seqread" || a.workload == "randwrite" ||
	             a.workload == "seqwrite" || a.workload == "walsync";
	if (a.pool.empty() || !known || (a.api != "sync" && a.api != "async") || !a.threads || !a.qd) {
		Usage();
	}
	if (!a.bs) {
		a.bs = a.workload == "randread" || a.workload == "randwrite" || a.workload == "walsync" ? 4096 : 256 << 10;
	}
	if (a.bs % SECTOR || a.size % a.bs || a.size < a.bs * a.threads) {
		fprintf(stderr, "xnvme-fs-bench: --bs is a multiple of 4 KiB, and --size a multiple of it holding a block per "
		                "thread\n");
		exit(64);
	}
	if (a.api == "sync") {
		a.qd = 1;
	}
	return a;
}

//! Stamps every sector of a block with its own file offset, the rest of it a pattern of the same.
static void Stamp(uint8_t *buf, uint64_t nbytes, uint64_t offset) {
	for (uint64_t s = 0; s < nbytes; s += SECTOR) {
		uint64_t at = offset + s;
		for (uint64_t i = 0; i < SECTOR; i += 8) {
			uint64_t v = at ^ (i * 0x9e3779b97f4a7c15ULL);
			memcpy(buf + s + i, i ? &v : &at, 8);
		}
	}
}

//! The first sector whose stamp is not its own offset, or UINT64_MAX when all are.
static uint64_t Check(const uint8_t *buf, uint64_t nbytes, uint64_t offset) {
	std::vector<uint8_t> want(SECTOR);
	for (uint64_t s = 0; s < nbytes; s += SECTOR) {
		Stamp(want.data(), SECTOR, offset + s);
		if (memcmp(buf + s, want.data(), SECTOR) != 0) {
			return offset + s;
		}
	}
	return UINT64_MAX;
}

static double CpuSeconds() {
	rusage ru;
	getrusage(RUSAGE_SELF, &ru);
	return double(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) + double(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e6;
}

struct ThreadResult {
	uint64_t ops = 0;
	uint64_t bytes = 0;
	uint64_t errors = 0;
	uint64_t bad_sector = UINT64_MAX;
	std::vector<double> latency_us;
};

static void Run(const Args &a, File &file, unsigned t, ThreadResult &res, Clock::time_point deadline) {
	bool write = a.workload == "randwrite" || a.workload == "seqwrite" || a.workload == "walsync";
	bool random = a.workload == "randread" || a.workload == "randwrite";
	uint64_t blocks = a.size / a.bs;
	// Each thread owns a contiguous share of the file, so sequential threads do not overlap
	uint64_t share = blocks / a.threads;
	uint64_t first = t * share;
	uint64_t next = 0;
	uint64_t rng = 0x2545F4914F6CDD1DULL ^ (uint64_t(t) + 1) * 0x9E3779B97F4A7C15ULL;
	auto pick = [&]() -> uint64_t {
		if (random) {
			rng ^= rng << 13;
			rng ^= rng >> 7;
			rng ^= rng << 17;
			return (first + rng % share) * a.bs;
		}
		return (first + next++ % share) * a.bs;
	};
	uint64_t ops_left = a.ops ? (a.ops + a.threads - 1) / a.threads : UINT64_MAX;
	std::vector<std::vector<uint8_t>> bufs(a.qd, std::vector<uint8_t>(a.bs));
	auto record = [&](Clock::time_point began, uint64_t offset, uint8_t *buf, bool ok, uint64_t moved) {
		res.latency_us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - began).count());
		res.ops++;
		res.bytes += moved;
		if (!ok) {
			res.errors++;
		} else if (a.verify && !write && res.bad_sector == UINT64_MAX) {
			res.bad_sector = Check(buf, a.bs, offset);
		}
	};

	if (a.api == "sync") {
		auto buf = bufs[0].data();
		while (ops_left-- && Clock::now() < deadline) {
			uint64_t offset = pick();
			auto began = Clock::now();
			bool ok = true;
			try {
				if (write) {
					if (a.verify) {
						Stamp(buf, a.bs, offset);
					}
					file.Write(buf, a.bs, offset);
					if (a.workload == "walsync") {
						file.Sync();
					}
				} else {
					ok = file.Read(buf, a.bs, offset) == a.bs;
				}
			} catch (std::system_error &) {
				ok = false;
			}
			record(began, offset, buf, ok, a.bs);
		}
		return;
	}

	// Asynchronous: keep qd requests in flight, polling them in turn and refilling each as it completes
	std::vector<::xnvme_fs::req> reqs;
	for (unsigned i = 0; i < a.qd; i++) {
		reqs.push_back(::xnvme_fs::req_new());
	}
	std::vector<uint64_t> offsets(a.qd);
	std::vector<Clock::time_point> began(a.qd);
	std::vector<bool> busy(a.qd, false);
	unsigned inflight = 0;
	bool stop = false;
	while (!stop || inflight) {
		for (unsigned i = 0; i < a.qd; i++) {
			if (busy[i]) {
				if (!reqs[i].poll()) {
					continue;
				}
				busy[i] = false;
				inflight--;
				uint64_t moved = 0;
				try {
					moved = reqs[i].wait();
				} catch (std::system_error &) { // NOLINT: counted as an error by record
				}
				record(began[i], offsets[i], bufs[i].data(), moved == a.bs, moved);
				if (a.workload == "walsync") {
					try {
						file.Sync();
					} catch (std::system_error &) {
						res.errors++;
					}
				}
			}
			if (!stop && (!ops_left || Clock::now() >= deadline)) {
				stop = true;
			}
			if (stop) {
				continue;
			}
			ops_left--;
			offsets[i] = pick();
			if (write && a.verify) {
				Stamp(bufs[i].data(), a.bs, offsets[i]);
			}
			began[i] = Clock::now();
			file.Handle().submit(reqs[i], write, bufs[i].data(), a.bs, offsets[i]);
			busy[i] = true;
			inflight++;
		}
	}
}

static double Percentile(std::vector<double> &v, double p) {
	if (v.empty()) {
		return 0;
	}
	size_t i = std::min(v.size() - 1, size_t(p / 100.0 * double(v.size())));
	std::nth_element(v.begin(), v.begin() + i, v.end());
	return v[i];
}

int main(int argc, char **argv) {
	auto a = Parse(argc, argv);
	try {
		auto session = std::make_shared<Session>(a.pool, a.socket_dir, "", a.homi_id);
		bool write = a.workload == "randwrite" || a.workload == "seqwrite" || a.workload == "walsync";
		auto file = session->Open(a.file, XNVME_FS_O_CREATE);

		// Reads need a file to read: written once, stamped, in large blocks
		if (!write && file->Size() < a.size) {
			std::vector<uint8_t> buf(1 << 20);
			for (uint64_t off = 0; off < a.size; off += buf.size()) {
				uint64_t n = std::min<uint64_t>(buf.size(), a.size - off);
				Stamp(buf.data(), n, off);
				file->Write(buf.data(), n, off);
			}
			file->Sync();
		}

		std::vector<ThreadResult> results(a.threads);
		std::vector<std::thread> threads;
		auto deadline =
		    a.ops
		        ? Clock::time_point::max()
		        : Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(a.seconds));
		auto began = Clock::now();
		double cpu_began = CpuSeconds();
		for (unsigned t = 0; t < a.threads; t++) {
			threads.emplace_back([&, t]() { Run(a, *file, t, results[t], deadline); });
		}
		for (auto &th : threads) {
			th.join();
		}
		double elapsed = std::chrono::duration<double>(Clock::now() - began).count();
		double cpu = CpuSeconds() - cpu_began;
		if (write) {
			file->Sync();
		}

		ThreadResult all;
		for (auto &r : results) {
			all.ops += r.ops;
			all.bytes += r.bytes;
			all.errors += r.errors;
			all.bad_sector = std::min(all.bad_sector, r.bad_sector);
			all.latency_us.insert(all.latency_us.end(), r.latency_us.begin(), r.latency_us.end());
		}

		// After writing, every block any thread wrote is read back and checked
		if (a.verify && write) {
			std::vector<uint8_t> buf(a.bs);
			uint64_t written = std::min(a.size, file->Size());
			for (uint64_t off = 0; off < written && all.bad_sector == UINT64_MAX; off += a.bs) {
				if (file->Read(buf.data(), a.bs, off) == a.bs) {
					std::vector<uint8_t> zero(a.bs, 0);
					// Blocks no thread reached are holes and read as zeros; anything else must be stamped
					if (memcmp(buf.data(), zero.data(), a.bs) != 0) {
						all.bad_sector = Check(buf.data(), a.bs, off);
					}
				}
			}
		}

		double p50 = Percentile(all.latency_us, 50);
		double p99 = Percentile(all.latency_us, 99);
		double p999 = Percentile(all.latency_us, 99.9);
		double iops = double(all.ops) / elapsed;
		double mbps = double(all.bytes) / elapsed / 1e6;
		bool verified = a.verify && all.bad_sector == UINT64_MAX;
		char json[1024];
		snprintf(json, sizeof(json),
		         "{\"workload\": \"%s\", \"api\": \"%s\", \"bs\": %llu, \"qd\": %u, \"threads\": %u, "
		         "\"seconds\": %.3f, \"ops\": %llu, \"errors\": %llu, \"iops\": %.1f, \"mb_per_s\": %.1f, "
		         "\"lat_us_p50\": %.1f, \"lat_us_p99\": %.1f, \"lat_us_p999\": %.1f, \"cpu_us_per_op\": %.2f, "
		         "\"verify\": %s}\n",
		         a.workload.c_str(), a.api.c_str(), (unsigned long long)a.bs, a.qd, a.threads, elapsed,
		         (unsigned long long)all.ops, (unsigned long long)all.errors, iops, mbps, p50, p99, p999,
		         all.ops ? cpu * 1e6 / double(all.ops) : 0.0,
		         !a.verify  ? "\"off\""
		         : verified ? "\"ok\""
		                    : "\"failed\"");
		fputs(json, stdout);
		if (!a.json.empty()) {
			FILE *f = fopen(a.json.c_str(), "w");
			if (f) {
				fputs(json, f);
				fclose(f);
			}
		}
		if (a.verify && !verified) {
			fprintf(stderr, "xnvme-fs-bench: sector at %llu does not hold its stamp\n",
			        (unsigned long long)all.bad_sector);
			return 1;
		}
		return all.errors ? 1 : 0;
	} catch (std::system_error &err) {
		fprintf(stderr, "xnvme-fs-bench: %s\n", err.what());
		return 1;
	}
}
