// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
// SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
//
// SPDX-License-Identifier: BSD-3-Clause
//
// Copies a local file into a pool served by 'xnvme fs serve', or a pool's file out to a local one,
// byte for byte. Run on the server's host, with XNVME_FS_HOMI_ID set unless XNVME_FS_BACKEND is a
// kernel one, and with XNVME_FS_SOCKET_DIR set where the server's socket is not in /run/xnvme-fs.
// A file copied in is synced before the copy returns.
//
//   xnvme fs cp POOL in LOCAL NAME
//   xnvme fs cp POOL out NAME LOCAL

#include <libxnvme_fs.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHUNK (8 * 1024 * 1024)

// Where 'xnvme fs serve' and xnvme-fs@.service put a pool's socket
#define SOCKET_DIR "/run/xnvme-fs"

// The first failure of a copy: its errno, and for a call of libxnvme_fs.h the message that
// xnvme_fs_errmsg() returned after the call
struct failure {
	int err;
	char why[512];
};

static void
failed(struct failure *fail, int err)
{
	if (!fail->err) {
		fail->err = err;
	}
}

static void
failed_fs(struct failure *fail, int err)
{
	if (!fail->err) {
		fail->err = err;
		snprintf(fail->why, sizeof(fail->why), "%s", xnvme_fs_errmsg());
	}
}

static int
report(const char *what, const struct failure *fail)
{
	if (!fail->err) {
		return 0;
	}
	fprintf(stderr, "xnvme fs cp: %s: %s%s%s\n", what, strerror(fail->err),
		fail->why[0] ? ": " : "", fail->why);
	return 1;
}

static int
In(struct xnvme_fs_session *s, const char *local, const char *name, char *buf)
{
	struct failure fail = {0};
	struct xnvme_fs_file *f = NULL;
	uint64_t at = 0;
	int err;
	int fd;

	fd = open(local, O_RDONLY);
	if (fd < 0) {
		failed(&fail, errno);
		return report(local, &fail);
	}
	err = xnvme_fs_open(s, name, XNVME_FS_O_CREATE | XNVME_FS_O_TRUNC, &f);
	if (err) {
		close(fd);
		failed_fs(&fail, -err);
		return report(name, &fail);
	}
	while (!fail.err) {
		ssize_t n = read(fd, buf, CHUNK);
		ssize_t w;

		if (n < 0) {
			failed(&fail, errno);
			close(fd);
			xnvme_fs_close(f);
			return report(local, &fail);
		}
		if (n == 0) {
			break;
		}
		w = xnvme_fs_pwrite(f, buf, (uint64_t)n, at);
		if (w < 0) {
			failed_fs(&fail, (int)-w);
		}
		at += (uint64_t)n;
	}
	close(fd);
	if (!fail.err) {
		err = xnvme_fs_fsync(f);
		if (err) {
			failed_fs(&fail, -err);
		}
	}
	err = xnvme_fs_close(f);
	if (err) {
		failed_fs(&fail, -err);
	}
	return report(name, &fail);
}

static int
Out(struct xnvme_fs_session *s, const char *name, const char *local, char *buf)
{
	struct failure fail = {0};
	struct xnvme_fs_file *f = NULL;
	int64_t size;
	int err;
	int fd;

	err = xnvme_fs_open(s, name, 0, &f);
	if (err) {
		failed_fs(&fail, -err);
		return report(name, &fail);
	}
	fd = open(local, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		failed(&fail, errno);
		xnvme_fs_close(f);
		return report(local, &fail);
	}
	size = xnvme_fs_size(f);
	if (size < 0) {
		failed_fs(&fail, (int)-size);
	}
	for (uint64_t at = 0; !fail.err && at < (uint64_t)size;) {
		uint64_t want = (uint64_t)size - at < CHUNK ? (uint64_t)size - at : CHUNK;
		ssize_t n = xnvme_fs_pread(f, buf, want, at);

		if (n < 0) {
			failed_fs(&fail, (int)-n);
		} else if (n == 0) {
			failed(&fail, EIO);
		} else if (write(fd, buf, (size_t)n) != n) {
			failed(&fail, errno ? errno : EIO);
		} else {
			at += (uint64_t)n;
		}
	}
	if (close(fd)) {
		failed(&fail, errno);
	}
	xnvme_fs_close(f);
	return report(name, &fail);
}

int
xnvme_fs_cp_main(int argc, char **argv)
{
	const char *dir = getenv("XNVME_FS_SOCKET_DIR");
	const char *homi = getenv("XNVME_FS_HOMI_ID");
	struct failure fail = {0};
	struct xnvme_fs_session *s = NULL;
	char *buf;
	int err;
	int rc;

	if (argc != 5 || (strcmp(argv[2], "in") && strcmp(argv[2], "out"))) {
		fprintf(stderr,
			"usage: xnvme fs cp POOL in LOCAL NAME | xnvme fs cp POOL out NAME LOCAL\n"
			"with XNVME_FS_HOMI_ID set unless XNVME_FS_BACKEND is a kernel one, and "
			"XNVME_FS_SOCKET_DIR where\nthe pool's socket is not in " SOCKET_DIR "\n");
		return 64;
	}
	err = xnvme_fs_connect(argv[1], dir && *dir ? dir : SOCKET_DIR, NULL,
			       homi ? (uint32_t)strtoul(homi, NULL, 10) : 0, &s);
	if (err) {
		failed_fs(&fail, -err);
		return report(argv[1], &fail);
	}
	buf = malloc(CHUNK);
	if (!buf) {
		xnvme_fs_disconnect(s);
		failed(&fail, ENOMEM);
		return report("buffer", &fail);
	}
	rc = strcmp(argv[2], "in") ? Out(s, argv[3], argv[4], buf) : In(s, argv[3], argv[4], buf);
	free(buf);
	xnvme_fs_disconnect(s);
	return rc;
}
