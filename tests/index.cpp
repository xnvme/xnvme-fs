// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
// SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
//
// SPDX-License-Identifier: BSD-3-Clause
//
// The index a reader outside the engines takes of a pool 'xnvme fs serve' serves: the files' sizes and extents as their
// writer leased and committed them, a watch answered only once another client changes the pool, and the extents of
// a removed file kept from reuse while a read lease older than the removal is held, given back by its release or by
// its connection closing. Run beside the server with XNVME_FS_SOCKET_DIR set.
//
//   xnvme-test-fs-index POOL

#include "error.hpp"
#include "protocol.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace xnvme_fs;

static int failures;

#define CHECK(cond)                                                                                                    \
	do {                                                                                                               \
		if (!(cond)) {                                                                                                 \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond);                                                 \
			failures++;                                                                                                \
		}                                                                                                              \
	} while (0)

static uint64_t FreeExtents(Client &c) {
	uint64_t n = 0;
	for (auto f : c.Info().free_extents) {
		n += f;
	}
	return n;
}

static const IndexedFile *Find(const Index &ix, const std::string &name) {
	for (auto &f : ix.files) {
		if (f.name == name) {
			return &f;
		}
	}
	return nullptr;
}

//! Writes 'name' with 'extents' leased and 'size' committed, and returns what was leased.
static std::vector<ExtentAddr> Write(Client &c, const std::string &name, uint32_t extents, uint64_t size) {
	auto o = c.Open(name, OPEN_CREATE | OPEN_TRUNCATE);
	auto got = c.Lease(o.handle, extents);
	c.Commit(o.handle, size);
	c.Close(o.handle, size);
	return got;
}

int main(int argc, char **argv) {
	const char *dir = getenv("XNVME_FS_SOCKET_DIR");
	if (argc != 2 || !dir) {
		fprintf(stderr, "usage: XNVME_FS_SOCKET_DIR=DIR xnvme-test-fs-index POOL\n");
		return 64;
	}
	auto socket = std::string(dir) + "/" + argv[1] + ".sock";
	try {
		Client writer(socket, argv[1]);
		Client reader(socket, argv[1]);
		auto extent = writer.Pool().extent_size;

		// Two extents and a bit committed: the index names three, the last one in part. Exactly three are leased, since
		// one leased past the end would be freed at a later table write, and held by the lease too
		auto leased = Write(writer, "index/a", 3, 2 * extent + 5);
		auto ix = reader.MakeIndex();
		auto a = Find(ix, "index/a");
		CHECK(a != nullptr);
		if (a) {
			CHECK(a->size == 2 * extent + 5);
			CHECK(a->extents == leased);
		}
		CHECK(ix.extent_size == extent);
		CHECK(ix.members.size() == writer.Pool().members.size());
		for (auto &m : ix.members) {
			CHECK(m.stripe == 0 ? m.places.size() == 1 && m.places[0].drive == m.path : m.places.size() > 1);
		}

		std::atomic<uint64_t> recalled {0};
		std::thread watch([&] { recalled = reader.Watch(ix.lease); });
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		CHECK(recalled == 0);
		Write(writer, "index/b", 1, 1);
		watch.join();
		CHECK(recalled > ix.version);

		auto free_before = FreeExtents(writer);
		writer.Remove("index/a");
		CHECK(FreeExtents(writer) == free_before);
		reader.Release(ix.lease);
		CHECK(FreeExtents(writer) == free_before + 3);
		try {
			reader.Watch(ix.lease);
			CHECK(!"a released lease is watched");
		} catch (Error &err) {
			CHECK(err.Code() == EINVAL);
		}

		// A lease taken after a removal holds nothing of it; one taken before holds until its connection closes
		free_before = FreeExtents(writer);
		{
			Client gone(socket, argv[1]);
			gone.MakeIndex();
			writer.Remove("index/b");
			CHECK(FreeExtents(writer) == free_before);
			reader.MakeIndex();
		}
		bool returned = false;
		for (int i = 0; i < 100 && !returned; i++) {
			returned = FreeExtents(writer) == free_before + 1;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		CHECK(returned);
	} catch (Error &err) {
		fprintf(stderr, "xnvme-test-fs-index: %s\n", err.what());
		return 1;
	}
	if (failures) {
		fprintf(stderr, "xnvme-test-fs-index: %d failures\n", failures);
		return 1;
	}
	printf("xnvme-test-fs-index: ok\n");
	return 0;
}
