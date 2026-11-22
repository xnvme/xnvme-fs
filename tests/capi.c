// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
// SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
//
// SPDX-License-Identifier: BSD-3-Clause
//
// The C interface of xnvme fs, from C: files opened, written, read back, truncated and synced on a
// pool 'xnvme fs serve' serves, requests submitted and polled, names made, listed, renamed and
// removed, also while open, and errors returned as negative errno with a message. Run beside the
// server, with XNVME_FS_SOCKET_DIR and XNVME_FS_HOMI_ID set, and with XNVME_FS_APPEND_BUFFER set
// to cover appends held in memory.
//
// Not covered: xnvme_fs_close() returning the errno of a failed write of held appends, since
// nothing in a guest makes a drive fail a write.
//
//   xnvme-test-fs-capi POOL

#ifndef _GNU_SOURCE
#define _GNU_SOURCE // memfd_create() and the file seals
#endif
#include <libxnvme_fs.h>

#ifdef XNVME_FS_DMABUF
#include <fcntl.h>
#include <linux/udmabuf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#endif
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

#define CHECK(cond)                                                                \
	do {                                                                       \
		if (!(cond)) {                                                     \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                                \
		}                                                                  \
	} while (0)

#define MIB (1024 * 1024)

/**
 * Pages through 'dir' 'max' entries a call, as an iterator does, appending "name:kind " to
 * 'names'; returns the number of entries, or the negative errno of the call that failed
 */
static int
list_all(struct xnvme_fs_session *s, const char *dir, uint32_t max, char *names, size_t len)
{
	struct xnvme_fs_entry entries[8];
	uint64_t offset = 0, remaining = 0, version = 0;
	uint32_t count = 0;

	names[0] = '\0';
	do {
		int err =
			xnvme_fs_list(s, dir, offset, entries, max, &count, &remaining, &version);

		if (err) {
			return err;
		}
		for (uint32_t i = 0; i < count; ++i) {
			size_t used = strlen(names);

			snprintf(names + used, len - used, "%s:%u ", entries[i].name,
				 entries[i].kind);
		}
		offset += count;
	} while (remaining);

	return (int)offset;
}

static pthread_key_t g_exit_key;
static int g_exit_checked;

// Runs as the thread exits, after the destructors of C++ thread_local objects: a call that fails
// here must still leave its message
static void
at_thread_exit(void *arg)
{
	struct xnvme_fs_session *t = NULL;

	(void)arg;
	CHECK(xnvme_fs_connect("pool", NULL, NULL, 0, &t) == -EINVAL && t == NULL);
	CHECK(strstr(xnvme_fs_errmsg(), "'socket_dir' is NULL") != NULL);
	g_exit_checked = 1;
}

static void *
fail_then_exit(void *arg)
{
	struct xnvme_fs_session *t = NULL;

	(void)arg;
	pthread_setspecific(g_exit_key, &g_exit_key);
	CHECK(xnvme_fs_connect(NULL, "/nowhere", NULL, 0, &t) == -EINVAL && t == NULL);
	CHECK(strstr(xnvme_fs_errmsg(), "'pool' is NULL") != NULL);
	return NULL;
}

int
main(int argc, char **argv)
{
	const char *dir = getenv("XNVME_FS_SOCKET_DIR");
	const char *homi = getenv("XNVME_FS_HOMI_ID");
	if (argc != 2 || !dir) {
		fprintf(stderr, "usage: XNVME_FS_SOCKET_DIR=DIR [XNVME_FS_HOMI_ID=N] "
				"xnvme-test-fs-capi POOL\n");
		return 64;
	}
	struct xnvme_fs_session *s = NULL;
	struct xnvme_fs_file *f = NULL;
	int err;

	// Nothing has failed on this thread yet
	CHECK(xnvme_fs_errmsg()[0] == 0);

	err = xnvme_fs_connect(argv[1], dir, NULL, homi ? (uint32_t)strtoul(homi, NULL, 10) : 0,
			       &s);
	if (err) {
		fprintf(stderr, "xnvme-test-fs-capi: connect: %s: %s\n", strerror(-err),
			xnvme_fs_errmsg());
		return 1;
	}
	CHECK(s != NULL);

	// A file that does not exist is an error unless created, and leaves no file
	f = (struct xnvme_fs_file *)s;
	CHECK(xnvme_fs_open(s, "capi/missing", 0, &f) == -ENOENT && f == NULL);

	CHECK(xnvme_fs_open(s, "capi/data", XNVME_FS_O_CREATE | XNVME_FS_O_TRUNC, &f) == 0);
	CHECK(f != NULL);
	if (!f) {
		return 1;
	}
	CHECK(xnvme_fs_size(f) == 0);

	// Three MiB, so a request spans extents and, on a pool of several members, members
	size_t n = 3 * MIB;
	unsigned char *out = malloc(n);
	unsigned char *in = malloc(n);
	for (size_t i = 0; i < n; i++) {
		out[i] = (unsigned char)(i * 131 + 7);
	}
	CHECK(xnvme_fs_pwrite(f, out, n, 0) == (ssize_t)n);
	CHECK(xnvme_fs_size(f) == (int64_t)n);
	memset(in, 0, n);
	CHECK(xnvme_fs_pread(f, in, n, 0) == (ssize_t)n);
	CHECK(memcmp(in, out, n) == 0);

	// A read past the end is short, and one at the end returns nothing
	CHECK(xnvme_fs_pread(f, in, MIB, n - 100) == 100);
	CHECK(xnvme_fs_pread(f, in, MIB, n) == 0);

	// Unaligned, both ways
	CHECK(xnvme_fs_pwrite(f, "hello", 5, 4093) == 5);
	CHECK(xnvme_fs_pread(f, in, 5, 4093) == 5 && memcmp(in, "hello", 5) == 0);

	// Asynchronous: two requests in flight at once, each polled and waited for
	struct xnvme_fs_req *a = (struct xnvme_fs_req *)s;
	struct xnvme_fs_req *b = NULL;
	int done = -1;
	CHECK(xnvme_fs_req_new(NULL) == -EINVAL);
	CHECK(strstr(xnvme_fs_errmsg(), "xnvme_fs_req_new: 'r' is NULL") != NULL);
	CHECK(xnvme_fs_req_new(&a) == 0 && a != NULL && a != (struct xnvme_fs_req *)s);
	CHECK(xnvme_fs_req_new(&b) == 0 && b != NULL);
	if (!a || !b) {
		return 1;
	}
	unsigned char *ina = malloc(MIB);
	unsigned char *inb = malloc(MIB);
	CHECK(xnvme_fs_submit(f, a, 0, ina, MIB, 0) == 0);
	CHECK(xnvme_fs_submit(f, b, 0, inb, MIB, 2 * MIB) == 0);
	CHECK(xnvme_fs_poll(a, NULL) == -EINVAL);
	do {
		err = xnvme_fs_poll(a, &done);
	} while (!err && !done);
	CHECK(err == 0 && done == 1);
	// Done, the request stays done, and polling it again says so
	done = -1;
	CHECK(xnvme_fs_poll(a, &done) == 0 && done == 1);
	CHECK(xnvme_fs_wait(a) == MIB);
	CHECK(xnvme_fs_wait(b) == MIB);
	memcpy(out + 4093, "hello", 5);
	CHECK(memcmp(ina, out, MIB) == 0);
	CHECK(memcmp(inb, out + 2 * MIB, MIB) == 0);
	CHECK(xnvme_fs_submit(f, a, 1, out, MIB, 4 * MIB) == 0);
	CHECK(xnvme_fs_wait(a) == MIB);
	CHECK(xnvme_fs_size(f) == (int64_t)(5 * MIB));

	// A gap the write skipped reads as zeros
	CHECK(xnvme_fs_pread(f, in, 4096, 3 * MIB + 8192) == 4096);
	{
		int zeros = 1;
		for (int i = 0; i < 4096; i++) {
			zeros &= in[i] == 0;
		}
		CHECK(zeros);
	}

	CHECK(xnvme_fs_ftruncate(f, MIB) == 0);
	CHECK(xnvme_fs_size(f) == MIB);
	CHECK(xnvme_fs_fsync(f) == 0);
	CHECK(xnvme_fs_close(f) == 0);

	// Reopened, the file holds what was synced
	f = NULL;
	CHECK(xnvme_fs_open(s, "capi/data", 0, &f) == 0);
	CHECK(f != NULL && xnvme_fs_size(f) == MIB);
	if (f) {
		CHECK(xnvme_fs_pread(f, in, MIB, 0) == MIB && memcmp(in, out, MIB) == 0);

		// Cut within an extent and written past again: what lay beyond the cut reads as
		// zeros, not as before
		CHECK(xnvme_fs_ftruncate(f, 1000) == 0);
		CHECK(xnvme_fs_pwrite(f, "x", 1, 8000) == 1);
		memset(in, 0xff, 7000);
		CHECK(xnvme_fs_pread(f, in, 7000, 1000) == 7000);
		int zeros = 1;
		for (int i = 0; i < 7000; i++) {
			zeros &= in[i] == 0;
		}
		CHECK(zeros);

		// Grown by a truncate, then written within the growth past the extents it holds
		CHECK(xnvme_fs_ftruncate(f, 4 * MIB) == 0);
		memcpy(ina, out, MIB);
		CHECK(xnvme_fs_pwrite(f, out, MIB, 2 * MIB) == MIB);
		CHECK(memcmp(out, ina, MIB) == 0);
		memset(in, 0, MIB);
		CHECK(xnvme_fs_pread(f, in, MIB, 2 * MIB) == MIB && memcmp(in, ina, MIB) == 0);

		// Grown to within a block past the extents, then appended to
		CHECK(xnvme_fs_ftruncate(f, 6 * MIB + 100) == 0);
		CHECK(xnvme_fs_pwrite(f, "abc", 3, 6 * MIB + 100) == 3);
		memset(in, 0xff, 103);
		CHECK(xnvme_fs_pread(f, in, 103, 6 * MIB) == 103 &&
		      memcmp(in + 100, "abc", 3) == 0);
		{
			int zeros = 1;
			for (int i = 0; i < 100; i++) {
				zeros &= in[i] == 0;
			}
			CHECK(zeros);
		}
		xnvme_fs_close(f);
	}

	{
		size_t rec = 137, count = 3000, total = rec * count;
		unsigned char *log = malloc(total + 4096);
		unsigned char *back = malloc(total + 4096);
		for (size_t i = 0; i < total + 4096; i++) {
			log[i] = (unsigned char)(i * 7 + 3);
		}
		struct xnvme_fs_file *l = NULL;
		CHECK(xnvme_fs_open(s, "capi/log", XNVME_FS_O_CREATE | XNVME_FS_O_TRUNC, &l) == 0);
		CHECK(l != NULL);
		for (size_t i = 0; l && i < count; i++) {
			CHECK(xnvme_fs_pwrite(l, log + i * rec, rec, i * rec) == (ssize_t)rec);
		}
		CHECK(l && xnvme_fs_size(l) == (int64_t)total);
		CHECK(l && xnvme_fs_pread(l, back, total, 0) == (ssize_t)total &&
		      memcmp(back, log, total) == 0);
		if (l) {
			CHECK(xnvme_fs_fsync(l) == 0);
			xnvme_fs_close(l);
		}

		l = NULL;
		CHECK(xnvme_fs_open(s, "capi/log", 0, &l) == 0);
		CHECK(l != NULL && xnvme_fs_size(l) == (int64_t)total);
		CHECK(l && xnvme_fs_pwrite(l, log + total, 1000, total) == 1000);
		CHECK(l && xnvme_fs_pread(l, back, total + 1000, 0) == (ssize_t)(total + 1000));
		CHECK(memcmp(back, log, total + 1000) == 0);

		size_t end = total + 1000;
		CHECK(l && xnvme_fs_pwrite(l, "XYZ", 3, end - 10) == 3);
		memcpy(log + end - 10, "XYZ", 3);
		CHECK(l && xnvme_fs_pwrite(l, log + end, 500, end) == 500);
		CHECK(l && xnvme_fs_pread(l, back, end + 500, 0) == (ssize_t)(end + 500));
		CHECK(memcmp(back, log, end + 500) == 0);

		size_t cut = end - 2000;
		CHECK(l && xnvme_fs_ftruncate(l, cut) == 0);
		CHECK(l && xnvme_fs_pwrite(l, log + cut, 300, cut) == 300);
		CHECK(l && xnvme_fs_size(l) == (int64_t)(cut + 300));
		CHECK(l && xnvme_fs_pread(l, back, cut + 300, 0) == (ssize_t)(cut + 300));
		CHECK(memcmp(back, log, cut + 300) == 0);
		if (l) {
			xnvme_fs_close(l);
		}
		free(log);
		free(back);
	}

	{
		// Appends left buffered, read back, written under at a lower block, passed by an
		// append larger than a command, and written at the close
		size_t rec = 137, count = 40, big = MIB + 3, total = rec * count + big + rec;
		unsigned char *data = malloc(total);
		unsigned char *back = malloc(total);
		for (size_t i = 0; i < total; i++) {
			data[i] = (unsigned char)(i * 13 + 5);
		}
		struct xnvme_fs_file *l = NULL;
		CHECK(xnvme_fs_open(s, "capi/buffered", XNVME_FS_O_CREATE | XNVME_FS_O_TRUNC,
				    &l) == 0);
		CHECK(l != NULL);
		size_t at = 0;
		for (size_t i = 0; l && i < count; i++, at += rec) {
			CHECK(xnvme_fs_pwrite(l, data + at, rec, at) == (ssize_t)rec);
		}
		CHECK(l && xnvme_fs_pread(l, back, at, 0) == (ssize_t)at &&
		      memcmp(back, data, at) == 0);
		CHECK(l && xnvme_fs_pread(l, back, 50, at - 50) == 50 &&
		      memcmp(back, data + at - 50, 50) == 0);
		memcpy(data + 10, "under", 5);
		CHECK(l && xnvme_fs_pwrite(l, "under", 5, 10) == 5);
		CHECK(l && xnvme_fs_size(l) == (int64_t)at);
		CHECK(l && xnvme_fs_pwrite(l, data + at, big, at) == (ssize_t)big);
		at += big;
		CHECK(l && xnvme_fs_pwrite(l, data + at, rec, at) == (ssize_t)rec);
		at += rec;
		CHECK(l && xnvme_fs_pread(l, back, total, 0) == (ssize_t)total &&
		      memcmp(back, data, total) == 0);
		CHECK(l && xnvme_fs_fsync(l) == 0);
		CHECK(l && xnvme_fs_pwrite(l, data, rec, at) == (ssize_t)rec);
		CHECK(l && xnvme_fs_close(l) == 0);

		l = NULL;
		CHECK(xnvme_fs_open(s, "capi/buffered", 0, &l) == 0);
		CHECK(l != NULL && xnvme_fs_size(l) == (int64_t)(total + rec));
		CHECK(l && xnvme_fs_pread(l, back, total, 0) == (ssize_t)total &&
		      memcmp(back, data, total) == 0);
		CHECK(l && xnvme_fs_pread(l, back, rec, total) == (ssize_t)rec &&
		      memcmp(back, data, rec) == 0);
		if (l) {
			xnvme_fs_close(l);
		}
		free(data);
		free(back);
	}

	{
		// Direct reads into DMA memory: whole blocks across extents, the end's block, and
		// what is refused
		uint32_t bs = xnvme_fs_block_size(s);
		size_t len = 3 * MIB + 3 * bs + 100;
		unsigned char *data = malloc(len);
		unsigned char *dma = xnvme_fs_dma_alloc(s, 4 * MIB);
		for (size_t i = 0; i < len; i++) {
			// Not periodic in a block, so a read of the wrong block shows
			data[i] = (unsigned char)((i * 29 + 11) ^ (i >> 9) ^ (i >> 17));
		}
		struct xnvme_fs_file *d = NULL;
		CHECK(xnvme_fs_open(s, "capi/direct", XNVME_FS_O_CREATE | XNVME_FS_O_TRUNC, &d) ==
		      0);
		CHECK(d != NULL && dma != NULL && bs >= 512);
		// More DMA memory than the HOMI's heap holds: NULL, and errno says why
		errno = 0;
		CHECK(xnvme_fs_dma_alloc(s, 1ULL << 40) == NULL && errno == ENOMEM);
		if (d && dma) {
			CHECK(xnvme_fs_pwrite(d, data, len, 0) == (ssize_t)len);
			CHECK(xnvme_fs_fsync(d) == 0);
			memset(dma, 0, 4 * MIB);
			CHECK(xnvme_fs_submit_direct(d, a, dma, 2 * MIB, MIB - bs) == 0);
			// Under way before anyone waits on it, so that the caller's work in
			// between overlaps it
			for (int ms = 0; ms < 1000 && memcmp(dma, data + MIB - bs, 2 * MIB) != 0;
			     ms++) {
				usleep(1000);
			}
			CHECK(memcmp(dma, data + MIB - bs, 2 * MIB) == 0);
			CHECK(xnvme_fs_wait(a) == 2 * MIB &&
			      memcmp(dma, data + MIB - bs, 2 * MIB) == 0);
			// The block holding the end, whole: the file's bytes, then the drive's
			CHECK(xnvme_fs_submit_direct(d, a, dma, 4 * bs, 3 * MIB) == 0);
			CHECK(xnvme_fs_wait(a) == 4 * bs &&
			      memcmp(dma, data + 3 * MIB, 3 * bs + 100) == 0);
			CHECK(xnvme_fs_remove(s, "capi/none") == -ENOENT);
			CHECK(xnvme_fs_submit_direct(d, a, dma, bs, 1) == 0 &&
			      xnvme_fs_wait(a) == -EINVAL);
			// The message is the request's, not the failure before it
			CHECK(strstr(xnvme_fs_errmsg(), "whole blocks") != NULL);
			CHECK(xnvme_fs_submit_direct(d, a, dma, bs + 1, 0) == 0 &&
			      xnvme_fs_wait(a) == -EINVAL);
			CHECK(xnvme_fs_submit_direct(d, a, dma, 5 * bs, 3 * MIB) == 0 &&
			      xnvme_fs_wait(a) == -EINVAL);
			// Appended to within the end's block and held in memory, which a direct
			// read cannot have
			if (getenv("XNVME_FS_APPEND_BUFFER")) {
				CHECK(xnvme_fs_pwrite(d, "tail", 4, len) == 4);
				CHECK(xnvme_fs_submit_direct(d, a, dma, 4 * bs, 3 * MIB) == 0 &&
				      xnvme_fs_wait(a) == -EINVAL);
				CHECK(xnvme_fs_submit_direct(d, a, dma, 3 * bs, 3 * MIB) == 0 &&
				      xnvme_fs_wait(a) == 3 * bs);
			}
			// Grown by a truncate past its extents: a hole
			CHECK(xnvme_fs_ftruncate(d, 8 * MIB) == 0);
			CHECK(xnvme_fs_submit_direct(d, a, dma, MIB, 6 * MIB) == 0 &&
			      xnvme_fs_wait(a) == -EINVAL);
			xnvme_fs_close(d);
		}
		// A descriptor that names no dma-buf: xNVMe returns -EINVAL for it, and a build without
		// dma-bufs returns -ENOTSUP
#ifdef XNVME_FS_DMABUF
		CHECK(xnvme_fs_register_dmabuf(s, -1, dma) == -EINVAL);
#else
		CHECK(xnvme_fs_register_dmabuf(s, -1, dma) == -ENOTSUP);
#endif
		if (dma) {
			xnvme_fs_dma_free(s, dma);
		}
		free(data);
	}

#ifdef XNVME_FS_DMABUF
	{
		// A direct read into a udmabuf registered under a PROT_NONE range, which faults on any
		// access; XNVME_FS_DMABUF_REQUIRED makes each skip a failure
		uint32_t bs = xnvme_fs_block_size(s);
		size_t len = 3 * MIB + 3 * bs + 100;
		unsigned char *data = malloc(len);
		void *mapped = MAP_FAILED, *reserved = MAP_FAILED;
		int udev = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
		int memfd = -1, fd = -1, err;
		struct xnvme_fs_file *d = NULL;

		for (size_t i = 0; data && i < len; i++) {
			data[i] = (unsigned char)((i * 29 + 11) ^ (i >> 9) ^ (i >> 17));
		}
		if (udev < 0) {
			CHECK(!getenv("XNVME_FS_DMABUF_REQUIRED"));
			printf("skipped: a direct read into a dma-buf, no /dev/udmabuf\n");
		} else {
			struct udmabuf_create create = {0};

			memfd = memfd_create("xnvme-fs-capi", MFD_ALLOW_SEALING | MFD_CLOEXEC);
			CHECK(memfd >= 0 && ftruncate(memfd, 2 * MIB) == 0 &&
			      fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK) == 0);
			create.memfd = memfd;
			create.size = 2 * MIB;
			fd = memfd >= 0 ? ioctl(udev, UDMABUF_CREATE, &create) : -1;
			CHECK(fd >= 0);
			mapped = mmap(NULL, 2 * MIB, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
			reserved = mmap(NULL, 2 * MIB, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			CHECK(data && mapped != MAP_FAILED && reserved != MAP_FAILED);
			CHECK(xnvme_fs_open(s, "capi/direct", 0, &d) == 0 && d != NULL);
		}
		if (d && fd >= 0 && data && mapped != MAP_FAILED && reserved != MAP_FAILED) {
			err = xnvme_fs_register_dmabuf(s, fd, reserved);
			if (err && !getenv("XNVME_FS_DMABUF_REQUIRED")) {
				printf("skipped: a direct read into a dma-buf, registering it: %s (%s)\n",
				       strerror(-err), xnvme_fs_errmsg());
			} else {
				CHECK(err == 0);
				memset(mapped, 0, 2 * MIB);
				CHECK(err == 0 &&
				      xnvme_fs_submit_direct(d, a, reserved, 2 * MIB, MIB - bs) == 0 &&
				      xnvme_fs_wait(a) == 2 * MIB);
				CHECK(memcmp(mapped, data + MIB - bs, 2 * MIB) == 0);
				CHECK(err || xnvme_fs_unregister_dmabuf(s, reserved) == 0);
			}
		}
		if (d) {
			xnvme_fs_close(d);
		}
		if (reserved != MAP_FAILED) {
			munmap(reserved, 2 * MIB);
		}
		if (mapped != MAP_FAILED) {
			munmap(mapped, 2 * MIB);
		}
		if (fd >= 0) {
			close(fd);
		}
		if (memfd >= 0) {
			close(memfd);
		}
		if (udev >= 0) {
			close(udev);
		}
		free(data);
	}
#endif

	// Names: a directory made, its entries listed, a file renamed while open, then removed
	{
		struct xnvme_fs_stat st;
		struct xnvme_fs_entry e[2];
		uint64_t remaining = 0, version = 0, seen = 0;
		uint32_t count = 0;
		char names[512];
		char got[8] = {0};

		CHECK(xnvme_fs_mkdir(s, "capi-names") == 0);
		CHECK(xnvme_fs_mkdir(s, "capi-names/sub") == 0);
		CHECK(xnvme_fs_stat(s, "capi-names", &st) == 0 &&
		      st.kind == XNVME_FS_KIND_DIRECTORY);

		struct xnvme_fs_file *n = NULL;
		CHECK(xnvme_fs_open(s, "capi-names/a", XNVME_FS_O_CREATE | XNVME_FS_O_TRUNC, &n) ==
		      0);
		CHECK(n != NULL);
		if (n) {
			CHECK(xnvme_fs_pwrite(n, "hello", 5, 0) == 5);
			// Open here, the size is the file's, not the server's as of its last
			// commit
			CHECK(xnvme_fs_stat(s, "capi-names/a", &st) == 0 &&
			      st.kind == XNVME_FS_KIND_FILE && st.size == 5);
			CHECK(xnvme_fs_fsync(n) == 0);

			// One entry a call, as the simplest iterator; the size of the file open
			// here is its size here
			CHECK(xnvme_fs_list(s, "capi-names", 0, e, 1, &count, &remaining,
					    &version) == 0);
			CHECK(count == 1 && remaining == 1 && version != 0);
			CHECK(strcmp(e[0].name, "a") == 0 && e[0].kind == XNVME_FS_KIND_FILE &&
			      e[0].size == 5);
			seen = version;
			CHECK(xnvme_fs_list(s, "capi-names", 1, e, 1, &count, &remaining,
					    &version) == 0);
			CHECK(count == 1 && remaining == 0 && version == seen);
			CHECK(strcmp(e[0].name, "sub") == 0 &&
			      e[0].kind == XNVME_FS_KIND_DIRECTORY && e[0].size == 0);
			// Both at once; none past the end; max 0 counts, and needs no array
			version = 0;
			CHECK(xnvme_fs_list(s, "capi-names", 0, e, 2, &count, &remaining,
					    &version) == 0);
			CHECK(count == 2 && remaining == 0);
			CHECK(xnvme_fs_list(s, "capi-names", 5, e, 2, &count, &remaining,
					    &version) == 0);
			CHECK(count == 0 && remaining == 0);
			CHECK(xnvme_fs_list(s, "capi-names", 0, NULL, 0, &count, &remaining,
					    &version) == 0);
			CHECK(count == 0 && remaining == 2);
			CHECK(xnvme_fs_list(s, "capi-names", 0, NULL, 1, &count, &remaining,
					    &version) == -EINVAL);
			CHECK(list_all(s, "capi-nothere", 2, names, sizeof(names)) == -ENOENT);

			// A name added between two pages makes the next one stale
			version = 0;
			CHECK(xnvme_fs_list(s, "capi-names", 0, e, 1, &count, &remaining,
					    &version) == 0);
			CHECK(xnvme_fs_mkdir(s, "capi-names/c") == 0);
			CHECK(xnvme_fs_list(s, "capi-names", 1, e, 1, &count, &remaining,
					    &version) == -ESTALE);
			CHECK(count == 0 && strstr(xnvme_fs_errmsg(), "capi-names") != NULL);
			CHECK(xnvme_fs_rmdir(s, "capi-names/c") == 0);
			// A failed call leaves the version, so the caller starts over from 0
			CHECK(xnvme_fs_list(s, "capi-names", 1, e, 1, &count, &remaining,
					    &version) == -ESTALE);
			version = 0;
			CHECK(list_all(s, "capi-names", 1, names, sizeof(names)) == 2);
			CHECK(strcmp(names, "a:1 sub:2 ") == 0);

			CHECK(xnvme_fs_rename(s, "capi-names/a", "capi-names/b") == 0);
			CHECK(xnvme_fs_pwrite(n, "!", 1, 5) == 1);
			CHECK(xnvme_fs_close(n) == 0);
		}
		CHECK(xnvme_fs_stat(s, "capi-names/a", &st) == 0 && st.kind == 0);
		CHECK(xnvme_fs_stat(s, "capi-names/b", &st) == 0 &&
		      st.kind == XNVME_FS_KIND_FILE && st.size == 6);
		n = NULL;
		CHECK(xnvme_fs_open(s, "capi-names/b", 0, &n) == 0);
		CHECK(n != NULL);
		if (n) {
			CHECK(xnvme_fs_pread(n, got, 6, 0) == 6 && memcmp(got, "hello!", 6) == 0);
			CHECK(xnvme_fs_close(n) == 0);
		}

		CHECK(xnvme_fs_remove(s, "capi-names/b") == 0);
		CHECK(xnvme_fs_stat(s, "capi-names/b", &st) == 0 && st.kind == 0);
		CHECK(xnvme_fs_remove(s, "capi-names/b") == -ENOENT);
		CHECK(strstr(xnvme_fs_errmsg(), "capi-names/b") != NULL);
		CHECK(xnvme_fs_remove(s, "capi-names/sub") == -EISDIR);
		CHECK(xnvme_fs_rmdir(s, "capi-names") == 0);
		CHECK(xnvme_fs_stat(s, "capi-names/sub", &st) == 0 && st.kind == 0);
	}

	// A directory of more entries than a page, in the order of their names
	{
		char names[512];

		CHECK(xnvme_fs_mkdir(s, "capi-many") == 0);
		for (int i = 0; i < 11; ++i) {
			char name[32];

			snprintf(name, sizeof(name), "capi-many/d%02d", i);
			CHECK(xnvme_fs_mkdir(s, name) == 0);
		}
		CHECK(list_all(s, "capi-many", 3, names, sizeof(names)) == 11);
		CHECK(strcmp(names, "d00:2 d01:2 d02:2 d03:2 d04:2 d05:2 d06:2 d07:2 d08:2 d09:2 "
				    "d10:2 ") == 0);
		CHECK(list_all(s, "capi-many", 8, names, sizeof(names)) == 11);
		CHECK(xnvme_fs_rmdir(s, "capi-many") == 0);
	}

	// The pool, and its members a page at a time
	{
		struct xnvme_fs_info info = {0};
		struct xnvme_fs_member m[1];
		uint64_t offset = 0, remaining = 0;
		uint32_t count = 0;
		int bad = 0;

		CHECK(xnvme_fs_info(s, &info) == 0);
		CHECK(info.nmembers >= 1 && info.extent_size > 0 && info.files >= 1);
		do {
			CHECK(xnvme_fs_members(s, offset, m, 1, &count, &remaining) == 0);
			bad |= count != 1 || !m[0].path[0] || !m[0].size || !m[0].extents ||
			       m[0].free_extents > m[0].extents;
			offset += count;
		} while (remaining && count);
		CHECK(!bad && offset == info.nmembers);
		CHECK(xnvme_fs_members(s, 0, NULL, 0, &count, &remaining) == 0);
		CHECK(count == 0 && remaining == info.nmembers);
	}

	// Names open in the process: removed, removed with their directory, and renamed onto
	{
		struct xnvme_fs_file *a1 = NULL, *a2 = NULL, *b1 = NULL, *found = NULL;
		struct xnvme_fs_stat st;
		char got[8] = {0};
		int flags = XNVME_FS_O_CREATE | XNVME_FS_O_TRUNC;

		// Removed while open: the handle still reads and writes the file, and the name is
		// free for a new one
		CHECK(xnvme_fs_open(s, "capi-open/a", flags, &a1) == 0 && a1 != NULL);
		CHECK(a1 && xnvme_fs_pwrite(a1, "hello", 5, 0) == 5);
		CHECK(xnvme_fs_remove(s, "capi-open/a") == 0);
		CHECK(xnvme_fs_stat(s, "capi-open/a", &st) == 0 && st.kind == 0 && st.size == 0);
		CHECK(a1 && xnvme_fs_pwrite(a1, "!", 1, 5) == 1);
		CHECK(a1 && xnvme_fs_pread(a1, got, 6, 0) == 6 && memcmp(got, "hello!", 6) == 0);
		CHECK(xnvme_fs_open(s, "capi-open/a", 0, &a2) == -ENOENT && a2 == NULL);
		CHECK(xnvme_fs_open(s, "capi-open/a", XNVME_FS_O_CREATE, &a2) == 0 && a2 != NULL);
		CHECK(a2 && a2 != a1 && xnvme_fs_size(a2) == 0);
		CHECK(a1 && xnvme_fs_size(a1) == 6);
		CHECK(a1 && xnvme_fs_fsync(a1) == 0);
		CHECK(a1 && xnvme_fs_close(a1) == 0);

		// Renamed onto itself: still found by its name
		CHECK(a2 && xnvme_fs_pwrite(a2, "aaaa", 4, 0) == 4);
		CHECK(xnvme_fs_rename(s, "capi-open/a", "capi-open/a") == 0);
		CHECK(xnvme_fs_stat(s, "capi-open/a", &st) == 0 && st.size == 4);

		// A second file renamed onto the open name: the first handle keeps the file it
		// opened, which no name has any longer, and the second handle's file has the name
		CHECK(xnvme_fs_open(s, "capi-open/b", flags, &b1) == 0 && b1 != NULL);
		CHECK(b1 && xnvme_fs_pwrite(b1, "bb", 2, 0) == 2);
		CHECK(xnvme_fs_rename(s, "capi-open/b", "capi-open/a") == 0);
		CHECK(xnvme_fs_stat(s, "capi-open/b", &st) == 0 && st.kind == 0);
		CHECK(xnvme_fs_stat(s, "capi-open/a", &st) == 0 && st.kind == XNVME_FS_KIND_FILE &&
		      st.size == 2);
		CHECK(a2 && xnvme_fs_pwrite(a2, "A", 1, 4) == 1 && xnvme_fs_size(a2) == 5);
		CHECK(a2 && xnvme_fs_pread(a2, got, 5, 0) == 5 && memcmp(got, "aaaaA", 5) == 0);
		CHECK(b1 && xnvme_fs_pwrite(b1, "B", 1, 2) == 1);
		// The size of the name is that of the file open under it, not yet synced
		CHECK(xnvme_fs_stat(s, "capi-open/a", &st) == 0 && st.size == 3);
		CHECK(xnvme_fs_open(s, "capi-open/a", 0, &found) == 0 && found != NULL);
		CHECK(found && xnvme_fs_pread(found, got, 8, 0) == 3 &&
		      memcmp(got, "bbB", 3) == 0);
		CHECK(found && xnvme_fs_close(found) == 0);
		CHECK(a2 && xnvme_fs_close(a2) == 0);
		CHECK(b1 && xnvme_fs_fsync(b1) == 0);
		CHECK(b1 && xnvme_fs_close(b1) == 0);
		found = NULL;
		CHECK(xnvme_fs_open(s, "capi-open/a", 0, &found) == 0 && found != NULL);
		CHECK(found && xnvme_fs_pread(found, got, 8, 0) == 3 &&
		      memcmp(got, "bbB", 3) == 0);

		// Removed with its directory while open: the name is gone, for xnvme_fs_stat() and
		// xnvme_fs_open() too, and the handle still reads the file
		CHECK(xnvme_fs_rmdir(s, "capi-open") == 0);
		CHECK(xnvme_fs_stat(s, "capi-open/a", &st) == 0 && st.kind == 0 && st.size == 0);
		a1 = NULL;
		CHECK(xnvme_fs_open(s, "capi-open/a", 0, &a1) == -ENOENT && a1 == NULL);
		CHECK(found && xnvme_fs_pread(found, got, 8, 0) == 3 &&
		      memcmp(got, "bbB", 3) == 0);
		CHECK(found && xnvme_fs_close(found) == 0);
	}

	// A backend there is none of, and arguments that are NULL
	{
		struct xnvme_fs_session *t = (struct xnvme_fs_session *)s;
		struct xnvme_fs_file *none = (struct xnvme_fs_file *)s;
		struct xnvme_fs_stat st;
		uint64_t rem = 0, ver = 0;
		uint32_t cnt = 0;

		CHECK(xnvme_fs_connect(argv[1], dir, "nope", 0, &t) == -ENOTSUP && t == NULL);
		CHECK(strstr(xnvme_fs_errmsg(), "'nope'") != NULL);

		CHECK(xnvme_fs_connect(NULL, dir, NULL, 0, &t) == -EINVAL);
		CHECK(strstr(xnvme_fs_errmsg(), "xnvme_fs_connect: 'pool' is NULL") != NULL);
		CHECK(xnvme_fs_connect(argv[1], NULL, NULL, 0, &t) == -EINVAL);
		CHECK(xnvme_fs_connect(argv[1], dir, NULL, 0, NULL) == -EINVAL);
		CHECK(xnvme_fs_open(s, NULL, 0, &none) == -EINVAL && none == NULL);
		CHECK(xnvme_fs_open(s, "capi/data", 0, NULL) == -EINVAL);
		CHECK(xnvme_fs_stat(s, NULL, &st) == -EINVAL);
		CHECK(xnvme_fs_stat(s, "capi/data", NULL) == -EINVAL);
		CHECK(xnvme_fs_list(s, NULL, 0, NULL, 0, &cnt, &rem, &ver) == -EINVAL);
		CHECK(xnvme_fs_list(s, "", 0, NULL, 0, NULL, &rem, &ver) == -EINVAL);
		CHECK(xnvme_fs_list(s, "", 0, NULL, 0, &cnt, NULL, &ver) == -EINVAL);
		CHECK(xnvme_fs_list(s, "", 0, NULL, 0, &cnt, &rem, NULL) == -EINVAL);
		CHECK(xnvme_fs_members(s, 0, NULL, 1, &cnt, &rem) == -EINVAL);
		CHECK(xnvme_fs_mkdir(s, NULL) == -EINVAL);
		CHECK(xnvme_fs_rmdir(s, NULL) == -EINVAL);
		CHECK(xnvme_fs_remove(s, NULL) == -EINVAL);
		CHECK(xnvme_fs_rename(s, NULL, "capi/x") == -EINVAL);
		CHECK(xnvme_fs_rename(s, "capi/data", NULL) == -EINVAL);
		CHECK(strstr(xnvme_fs_errmsg(), "xnvme_fs_rename: 'to' is NULL") != NULL);
		CHECK(xnvme_fs_info(s, NULL) == -EINVAL);
	}

	// A call that fails as its thread exits leaves its message as any other does
	{
		pthread_t thread;

		CHECK(pthread_key_create(&g_exit_key, at_thread_exit) == 0);
		CHECK(pthread_create(&thread, NULL, fail_then_exit, NULL) == 0);
		CHECK(pthread_join(thread, NULL) == 0);
		CHECK(g_exit_checked);
		// The message is the thread's own: this thread's is still its last failure
		CHECK(strstr(xnvme_fs_errmsg(), "xnvme_fs_info: 'info' is NULL") != NULL);
	}

	xnvme_fs_req_free(a);
	xnvme_fs_req_free(b);
	free(out);
	free(in);
	free(ina);
	free(inb);
	xnvme_fs_disconnect(s);
	if (failures) {
		fprintf(stderr, "xnvme-test-fs-capi: %d failed\n", failures);
		return 1;
	}
	printf("xnvme-test-fs-capi: ok\n");
	return 0;
}
