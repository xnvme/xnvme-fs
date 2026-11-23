// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
// SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
//
// SPDX-License-Identifier: BSD-3-Clause
//
// Opens a file of a pool 'xnvme fs serve' serves, writes TEXT at its start, syncs, prints "held" and keeps the file
// open until it is killed, for tests of what the server does with a file while a client holds it and once that client
// is gone without closing it. An open the server fails is printed as the server worded it. Run beside the server, with
// XNVME_FS_SOCKET_DIR and XNVME_FS_HOMI_ID set.
//
//   xnvme-test-fs-hold POOL NAME TEXT

#include "error.hpp"
#include "session.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

using namespace xnvme_fs;

int main(int argc, char **argv) {
	const char *dir = getenv("XNVME_FS_SOCKET_DIR");
	const char *homi = getenv("XNVME_FS_HOMI_ID");
	if (argc != 4 || !dir) {
		fprintf(stderr, "usage: XNVME_FS_SOCKET_DIR=DIR [XNVME_FS_HOMI_ID=N] xnvme-test-fs-hold POOL NAME TEXT\n");
		return 64;
	}
	try {
		Access access;
		access.homi_id = homi ? uint32_t(strtoul(homi, nullptr, 10)) : 0;
		auto session = std::make_shared<Session>(argv[1], std::string(dir) + "/" + argv[1] + ".sock", access);
		auto file = session->Open(argv[2], OPEN_CREATE);
		file->Write(argv[3], strlen(argv[3]), 0);
		file->Sync();
		printf("held\n");
		fflush(stdout);
		for (;;) {
			pause();
		}
	} catch (Error &err) {
		fprintf(stderr, "xnvme-test-fs-hold: %s\n", err.what());
		return 1;
	}
}
