// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
// SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
//
// SPDX-License-Identifier: BSD-3-Clause

#include "trace.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <mutex>
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace xnvme_fs {

namespace {
struct Tracer {
	int fd = -1;
	std::chrono::steady_clock::time_point began;

	Tracer() {
		auto path = getenv("XNVME_FS_TRACE");
		if (path && *path) {
			fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
		}
		began = std::chrono::steady_clock::now();
	}
};

Tracer &Get() {
	static Tracer tracer;
	return tracer;
}
} // namespace

bool Tracing() {
	return Get().fd >= 0;
}

void TraceOp(const char *op, const std::string &name, uint64_t offset, uint64_t nbytes) {
	auto &t = Get();
	if (t.fd < 0) {
		return;
	}
	double at = std::chrono::duration<double>(std::chrono::steady_clock::now() - t.began).count();
	char line[512];
	int n = snprintf(line, sizeof(line), "%.6f %d %ld %s %s %llu %llu\n", at, int(getpid()), long(syscall(SYS_gettid)),
	                 op, name.empty() ? "/" : name.c_str(), (unsigned long long)offset, (unsigned long long)nbytes);
	if (n > 0) {
		// O_APPEND makes the one write land whole after whatever another process appended
		ssize_t ignored = write(t.fd, line, size_t(n) < sizeof(line) ? size_t(n) : sizeof(line) - 1);
		(void)ignored;
	}
}

} // namespace xnvme_fs
