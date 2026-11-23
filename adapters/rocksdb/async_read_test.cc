// SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
//
// SPDX-License-Identifier: MIT
//
// ReadAsync with the request on a stack frame that is gone, and overwritten, before Poll calls back, as RocksDB's
// prefetch buffer starts its reads. The file system must call back with what it kept of the request. Takes the file
// system by TEST_FS_URI, as RocksDB's own tests do.

#include "rocksdb/convenience.h"
#include "rocksdb/env.h"
#include "rocksdb/file_system.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace ROCKSDB_NAMESPACE;

static constexpr size_t SIZE = 64 << 10;
static constexpr uint64_t OFFSET = 8 << 10;
static constexpr size_t LEN = 4 << 10;

struct Seen {
	bool called = false;
	uint64_t offset = 0;
	size_t len = 0;
	std::string data;
	bool ok = false;
};

static uint8_t Pattern(uint64_t at) {
	return uint8_t(at * 7 + 3);
}

__attribute__((noinline)) static IOStatus Start(FSRandomAccessFile &file, char *scratch, Seen &seen, void **handle,
                                                IOHandleDeleter *del) {
	FSReadRequest req;
	req.offset = OFFSET;
	req.len = LEN;
	req.scratch = scratch;
	auto cb = [](FSReadRequest &r, void *arg) {
		auto s = static_cast<Seen *>(arg);
		s->called = true;
		s->offset = r.offset;
		s->len = r.len;
		s->data.assign(r.result.data(), r.result.size());
		s->ok = r.status.ok();
	};
	return file.ReadAsync(req, IOOptions(), cb, &seen, handle, del, nullptr);
}

__attribute__((noinline)) static void Clobber() {
	volatile char junk[16 << 10];
	memset(const_cast<char *>(junk), 0xA5, sizeof(junk));
}

static int Fail(const char *what) {
	fprintf(stderr, "async_read_test: %s\n", what);
	return 1;
}

int main() {
	const char *uri = getenv("TEST_FS_URI");
	if (!uri) {
		return Fail("TEST_FS_URI is not set");
	}
	Env *env = nullptr;
	std::shared_ptr<Env> guard;
	if (!Env::CreateFromUri(ConfigOptions(), "", uri, &env, &guard).ok()) {
		return Fail("no file system for TEST_FS_URI");
	}
	auto fs = env->GetFileSystem();
	std::string dir = "/async_read_test";
	fs->CreateDirIfMissing(dir, IOOptions(), nullptr);
	std::string name = dir + "/data";

	std::vector<char> bytes(SIZE);
	for (size_t i = 0; i < SIZE; i++) {
		bytes[i] = char(Pattern(i));
	}
	std::unique_ptr<FSWritableFile> w;
	if (!fs->NewWritableFile(name, FileOptions(), &w, nullptr).ok() ||
	    !w->Append(Slice(bytes.data(), SIZE), IOOptions(), nullptr).ok() || !w->Sync(IOOptions(), nullptr).ok() ||
	    !w->Close(IOOptions(), nullptr).ok()) {
		return Fail("could not write the file");
	}

	std::unique_ptr<FSRandomAccessFile> r;
	if (!fs->NewRandomAccessFile(name, FileOptions(), &r, nullptr).ok()) {
		return Fail("could not open the file");
	}
	std::vector<char> scratch(LEN);
	Seen seen;
	void *handle = nullptr;
	IOHandleDeleter del;
	if (!Start(*r, scratch.data(), seen, &handle, &del).ok()) {
		return Fail("ReadAsync failed");
	}
	Clobber();
	std::vector<void *> handles {handle};
	if (!fs->Poll(handles, 1).ok()) {
		return Fail("Poll failed");
	}
	if (del) {
		del(handle);
	}

	if (!seen.called || !seen.ok) {
		return Fail("the read was not called back as done");
	}
	if (seen.offset != OFFSET || seen.len != LEN || seen.data.size() != LEN) {
		return Fail("the callback's request is not the one started");
	}
	for (size_t i = 0; i < LEN; i++) {
		if (uint8_t(seen.data[i]) != Pattern(OFFSET + i)) {
			return Fail("the callback's result does not hold the file's bytes");
		}
	}
	fs->DeleteFile(name, IOOptions(), nullptr);
	fs->DeleteDir(dir, IOOptions(), nullptr);
	printf("async_read_test: ok\n");
	return 0;
}
