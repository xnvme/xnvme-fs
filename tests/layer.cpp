// SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
//
// SPDX-License-Identifier: BSD-3-Clause
//
// The C++ layer yace generates from libxnvme_fs.h, and the adapters' classes on top of it, where no server
// runs: both compile as the C++ standard this program is built as, and a call that fails throws a
// std::system_error with the call's errno, whose what() names the C function and carries the message the library
// left for xnvme_fs_errmsg(). Calls that need a server are covered by the adapters' suites.
//
//   xnvme-test-fs-layer

#include "xnvme_fs.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <system_error>
#include <type_traits>

static int failures;

#define CHECK(cond)                                                                                                    \
	do {                                                                                                           \
		if (!(cond)) {                                                                                         \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond);                                     \
			failures++;                                                                                    \
		}                                                                                                      \
	} while (0)

// A handle frees what it owns once, so it moves and does not copy
static_assert(!std::is_copy_constructible<::xnvme_fs::session>::value, "a session does not copy");
static_assert(std::is_nothrow_move_constructible<::xnvme_fs::session>::value, "a session moves");
static_assert(!std::is_copy_constructible<::xnvme_fs::file>::value, "a file does not copy");
static_assert(std::is_nothrow_move_constructible<::xnvme_fs::req>::value, "a request moves");

int main() {
	// A pool nobody serves, through the generated layer and through the adapters' session
	try {
		::xnvme_fs::connect("nopool", "/nonexistent", "io_uring", 0);
		CHECK(!"connect returned");
	} catch (const std::system_error &err) {
		std::string what = err.what();
		CHECK(err.code().value() != 0 && err.code().category() == std::generic_category());
		CHECK(what.rfind("xnvme_fs_connect: ", 0) == 0);
		CHECK(what.find("'/nonexistent/nopool.sock'") != std::string::npos);
		CHECK(what.find(::xnvme_fs::errmsg()) != std::string::npos);
	}
	try {
		::xnvme::fs::Session session("nopool", "/nonexistent", "io_uring", 0);
		CHECK(!"Session returned");
	} catch (const std::system_error &err) {
		CHECK(strstr(err.what(), "'/nonexistent/nopool.sock'") != nullptr);
	}

	// A request needs no session: made, polled before it was ever submitted, and freed as it leaves scope
	try {
		auto made = ::xnvme_fs::req_new();
		CHECK(made.native_handle() != nullptr);
		auto moved = std::move(made);
		CHECK(made.native_handle() == nullptr && moved.native_handle() != nullptr);
	} catch (const std::system_error &err) {
		fprintf(stderr, "xnvme-test-fs-layer: %s\n", err.what());
		failures++;
	}

	if (failures) {
		fprintf(stderr, "xnvme-test-fs-layer: %d failed\n", failures);
		return 1;
	}
	printf("xnvme-test-fs-layer: ok\n");
	return 0;
}
