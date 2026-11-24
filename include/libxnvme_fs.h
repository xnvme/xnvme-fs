// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
// SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
//
// SPDX-License-Identifier: BSD-3-Clause

/**
 * xnvme fs from C: files on a pool served by `xnvme fs serve`, read and written by the calling
 * process itself, through uPCIe or the kernel's io_uring
 *
 * A session is the process's connection to one pool's server; a file is a name in that pool. The
 * server allocates each extent of a file on the member with the most free extents, so a read or a
 * write may span several members; the synchronous calls wait for it, the asynchronous ones return
 * at once and are polled by the thread that submitted them. Calls return 0, a count, or a negative
 * errno; xnvme_fs_dma_alloc() returns memory, or NULL with errno set. xnvme_fs_errmsg() returns
 * the message of the last call that failed on the thread. A call given NULL for a string argument
 * fails with -EINVAL, unless the call says what NULL means.
 */
#ifndef LIBXNVME_FS_H
#define LIBXNVME_FS_H

#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The process's connection to the server of one pool, from xnvme_fs_connect() */
struct xnvme_fs_session;
/** A file of the pool that this process has open, from xnvme_fs_open() */
struct xnvme_fs_file;
/** An asynchronous read or write, from xnvme_fs_req_new() */
struct xnvme_fs_req;

#define XNVME_FS_O_CREATE 1 ///< xnvme_fs_open() creates the file where there is none
#define XNVME_FS_O_TRUNC  2 ///< xnvme_fs_open() truncates the file to size 0
#define XNVME_FS_O_EXCL   4 ///< xnvme_fs_open() fails where the file exists

#define XNVME_FS_KIND_FILE      1 ///< The name is a file
#define XNVME_FS_KIND_DIRECTORY 2 ///< The name is a directory

/**
 * A name in the pool: 'kind' is 0 where there is no such name. For a file that is open through the
 * session given to xnvme_fs_stat(), 'size' is the file's size in that session, as xnvme_fs_size()
 * returns it. For any other file, 'size' is the size the server last received for the file, from
 * an xnvme_fs_fsync(), an xnvme_fs_ftruncate() or the close of the file's last handle in its
 * session; only xnvme_fs_fsync() makes that size durable. 'size' is 0 for a directory.
 */
struct xnvme_fs_stat {
	uint64_t size;
	uint8_t kind;
};

/** The pool as its server holds it */
struct xnvme_fs_info {
	uint32_t layout_version;
	uint32_t nmembers;
	uint64_t extent_size;
	uint64_t generation;
	uint64_t files;
};

/** The longest name of a file or directory in the pool, and of a member's path, without the NUL */
#define XNVME_FS_NAME_MAX 255

/**
 * An entry of a directory: 'size' is a file's size as 'size' of struct xnvme_fs_stat gives it, 0
 * for a directory
 */
struct xnvme_fs_entry {
	uint64_t size;
	uint8_t kind;
	uint8_t rsvd[7];
	char name[XNVME_FS_NAME_MAX + 1]; ///< The entry's name in its directory, NUL-terminated
};

/** One member of the pool: its size and extents, and how many of those are free */
struct xnvme_fs_member {
	uint64_t size;
	uint64_t extents;
	uint64_t free_extents;
	char path[XNVME_FS_NAME_MAX + 1]; ///< NUL-terminated
};

/**
 * The message of the call on this thread that failed last, or "" where none has
 *
 * The pointer is to storage of the calling thread, the same on every call: it stays valid until
 * the thread exits, and holds this message until the next call that fails on the thread. A call
 * that succeeds leaves the message as it was.
 */
const char *
xnvme_fs_errmsg(void);

/** Connects to the server of 'pool' at '<socket_dir>/<pool>.sock', its members reached with
 * 'backend', one of upcie, io_uring, io_uring_direct and io_uring_cmd, and upcie through HOMI
 * 'homi_id'; a NULL or empty 'backend' is XNVME_FS_BACKEND's, else upcie. Sets '*s' to the
 * session, or to NULL where the call fails
 *
 * @param pool The name of the pool
 * @param socket_dir The directory that holds the socket of the pool's server
 * @param backend How the members are reached, NULL or "" for the default
 * @param homi_id The HOMI that upcie reaches the members through
 * @param[out] s Set to the session
 *
 * @return 0 on success, a negative errno on failure
 *
 * @nullable backend
 * @errno
 */
int
xnvme_fs_connect(const char *pool, const char *socket_dir, const char *backend, uint32_t homi_id,
		 struct xnvme_fs_session **s);
/**
 * Frees 's'. Every request submitted on a file of 's' must be done before the call
 *
 * @param s The session
 *
 * @destroys s
 */
void
xnvme_fs_disconnect(struct xnvme_fs_session *s);

/**
 * Opens 'name' with the XNVME_FS_O_* in 'flags'; sets '*f' to the file, or to NULL where the call
 * fails
 *
 * @param s The session
 * @param name The name of the file in the pool
 * @param flags Bits of XNVME_FS_O_*
 * @param[out] f Set to the file
 *
 * @return 0 on success, a negative errno on failure
 *
 * @errno
 */
int
xnvme_fs_open(struct xnvme_fs_session *s, const char *name, uint32_t flags,
	      struct xnvme_fs_file **f);
/**
 * Writes the appends the file holds in memory and frees 'f' either way; returns 0, or -errno if
 * the write failed. Every request submitted on 'f' must be done before the call
 *
 * @param f The file
 *
 * @return 0 on success, a negative errno where the write failed
 *
 * @destroys f
 * @errno
 */
int
xnvme_fs_close(struct xnvme_fs_file *f);
/**
 * The size of the file in bytes as this process has it, appends held in memory included
 *
 * @param f The file
 *
 * @return The size, or a negative errno
 *
 * @errno
 */
int64_t
xnvme_fs_size(struct xnvme_fs_file *f);
/**
 * Reads up to 'nbytes' bytes of the file from 'offset' into 'buf', and waits for them
 *
 * @param f The file
 * @param[out] buf Where the bytes are read to
 * @param nbytes The number of bytes that 'buf' has room for
 * @param offset Where in the file the read starts
 *
 * @return The number of bytes read, fewer than 'nbytes' at the end of the file, or a negative
 * errno
 *
 * @len buf nbytes
 * @errno
 */
ssize_t
xnvme_fs_pread(struct xnvme_fs_file *f, void *buf, uint64_t nbytes, uint64_t offset);
/**
 * Writes the 'nbytes' bytes of 'buf' to the file at 'offset', and waits for them; where
 * XNVME_FS_APPEND_BUFFER is set, an append shorter than a command is held in memory instead, for
 * xnvme_fs_fsync() or xnvme_fs_close() to write
 *
 * @param f The file
 * @param[in] buf The bytes to write
 * @param nbytes The number of bytes to write
 * @param offset Where in the file the write starts
 *
 * @return 'nbytes', or a negative errno
 *
 * @len buf nbytes
 * @errno
 */
ssize_t
xnvme_fs_pwrite(struct xnvme_fs_file *f, const void *buf, uint64_t nbytes, uint64_t offset);
/**
 * Makes the file's data and size durable
 *
 * @param f The file
 *
 * @return 0 on success, a negative errno on failure
 *
 * @errno
 */
int
xnvme_fs_fsync(struct xnvme_fs_file *f);
/**
 * Sets the size of the file to 'size' bytes, after writing the appends held in memory: the bytes
 * past a smaller size are gone, and those up to a larger size read as zeros
 *
 * @param f The file
 * @param size The size in bytes
 *
 * @return 0 on success, a negative errno on failure
 *
 * @errno
 */
int
xnvme_fs_ftruncate(struct xnvme_fs_file *f, uint64_t size);

/**
 * Fills 'st' for 'name'; 0 also where there is no such name, with st->kind 0
 *
 * @param s The session
 * @param name The name in the pool
 * @param[out] st What the name is
 *
 * @return 0 on success, also where there is no such name, a negative errno on failure
 *
 * @errno
 */
int
xnvme_fs_stat(struct xnvme_fs_session *s, const char *name, struct xnvme_fs_stat *st);
/**
 * Fills 'entries' with the entries of 'directory', "" for the top, from the 'offset'th on, in the
 * order of their names
 *
 * A caller iterates by calling again with 'offset' advanced by 'count' until 'remaining' is 0. The
 * server returns at most 4096 entries a call, so 'count' may be less than 'max' while 'remaining'
 * is not 0; 'max' 0 gives the number of entries in 'remaining', and 'entries' may then be NULL.
 *
 * Names added, removed or renamed between two calls shift the offsets. Pass 'version' 0 on the
 * first call: it is set to the version of the pool's names, and a later call given that version
 * fails with -ESTALE once the names have changed, so the caller can start over. The size of an
 * entry is the size that xnvme_fs_stat() gives for the name.
 *
 * @param s The session
 * @param directory The directory, "" for the top
 * @param offset The number of entries to skip
 * @param[out] entries The entries, as many as 'count'
 * @param max The number of entries that 'entries' has room for
 * @param[out] count Set to the number of entries written
 * @param[out] remaining Set to the number of entries after those written
 * @param[in,out] version 0, or the version a previous call set
 *
 * @return 0 on success, -ENOENT where there is no such directory, -ESTALE where the names changed
 * since 'version', or another negative errno
 *
 * @nullable entries
 * @len entries max
 * @len entries count
 * @errno
 */
int
xnvme_fs_list(struct xnvme_fs_session *s, const char *directory, uint64_t offset,
	      struct xnvme_fs_entry *entries, uint32_t max, uint32_t *count, uint64_t *remaining,
	      uint64_t *version);
/**
 * Makes the directory 'name'; 0 also where the directory exists
 *
 * @param s The session
 * @param name The name of the directory in the pool
 *
 * @return 0 on success, -EEXIST where 'name' is a file, or another negative errno
 *
 * @errno
 */
int
xnvme_fs_mkdir(struct xnvme_fs_session *s, const char *name);
/**
 * Removes the directory 'name' and every file and directory under it; -EBUSY where one of the
 * files is open through another session, of this process or of another
 *
 * @param s The session
 * @param name The name of the directory in the pool
 *
 * @return 0 on success, -EBUSY where one of the files is open through another session, or another
 * negative errno
 *
 * @errno
 */
int
xnvme_fs_rmdir(struct xnvme_fs_session *s, const char *name);
/**
 * Removes the file 'name'; -EBUSY where the file is open through another session, of this process
 * or of another
 *
 * @param s The session
 * @param name The name of the file in the pool
 *
 * @return 0 on success, -EBUSY where the file is open through another session, or another negative
 * errno
 *
 * @errno
 */
int
xnvme_fs_remove(struct xnvme_fs_session *s, const char *name);
/**
 * Renames the file 'from' to 'to'; a file this process has open under 'from' is then 'to'
 *
 * @param s The session
 * @param from The name the file has
 * @param to The name the file gets
 *
 * @return 0 on success, a negative errno on failure
 *
 * @errno
 */
int
xnvme_fs_rename(struct xnvme_fs_session *s, const char *from, const char *to);
/**
 * Fills 'info' with the pool as its server holds it
 *
 * @param s The session
 * @param[out] info The pool
 *
 * @return 0 on success, a negative errno on failure
 *
 * @errno
 */
int
xnvme_fs_info(struct xnvme_fs_session *s, struct xnvme_fs_info *info);

/**
 * Fills 'members' with the pool's members, from the 'offset'th on, in the order of the pool
 *
 * As xnvme_fs_list() pages a directory; the members do not change while the pool is served, so
 * there is no version. xnvme_fs_info() gives their number as 'nmembers'.
 *
 * @param s The session
 * @param offset The number of members to skip
 * @param[out] members The members, as many as 'count'
 * @param max The number of members that 'members' has room for
 * @param[out] count Set to the number of members written
 * @param[out] remaining Set to the number of members after those written
 *
 * @return 0 on success, a negative errno on failure
 *
 * @nullable members
 * @len members max
 * @len members count
 * @errno
 */
int
xnvme_fs_members(struct xnvme_fs_session *s, uint64_t offset, struct xnvme_fs_member *members,
		 uint32_t max, uint32_t *count, uint64_t *remaining);

/**
 * Makes a request, which a submit starts, and may start again once the request is done
 *
 * @param[out] r Set to the request, for xnvme_fs_req_free() to free, or to NULL where the call
 * fails
 *
 * @return 0 on success, -ENOMEM where there is no memory, -EINVAL where 'r' is NULL
 *
 * @errno
 */
int
xnvme_fs_req_new(struct xnvme_fs_req **r);
/**
 * Frees 'r', which must be done, or never submitted
 *
 * @param r The request, NULL is ignored
 *
 * @nullable r
 * @destroys r
 */
void
xnvme_fs_req_free(struct xnvme_fs_req *r);
/**
 * Starts a read ('write' 0) or a write; a read's buffer must stay valid until the request is done
 *
 * The request is done once xnvme_fs_poll() sets 'done' to 1 or xnvme_fs_wait() returns. It must
 * be done before it is submitted again or freed, before xnvme_fs_close() of 'f', and before
 * xnvme_fs_disconnect() of the session. A request that could not be started is done, and
 * xnvme_fs_wait() returns why.
 *
 * @param f The file
 * @param r The request, which is done or was never submitted
 * @param write 0 for a read, anything else for a write
 * @param buf Where the bytes of a read go, once the request is done, or the bytes of a write
 * @param nbytes The number of bytes to read or write
 * @param offset Where in the file the read or write starts
 *
 * @return 0 on success, a negative errno on failure
 *
 * @errno
 */
int
xnvme_fs_submit(struct xnvme_fs_file *f, struct xnvme_fs_req *r, int write, void *buf,
		uint64_t nbytes, uint64_t offset);
/**
 * The pool's block size, which a direct read's offset and length are multiples of
 *
 * @param s The session
 *
 * @return The block size in bytes
 */
uint32_t
xnvme_fs_block_size(struct xnvme_fs_session *s);
/**
 * Allocates 'nbytes' bytes of memory that the drives transfer data to and from directly, from the
 * HOMI serving the pool, for xnvme_fs_dma_free() to free; NULL with errno set where there is none
 *
 * @param s The session
 * @param nbytes The number of bytes
 *
 * @return The memory, NULL with errno set where there is none
 *
 * @nullable return
 */
void *
xnvme_fs_dma_alloc(struct xnvme_fs_session *s, uint64_t nbytes);
/**
 * Frees 'p', memory that xnvme_fs_dma_alloc() returned for 's'
 *
 * @param s The session
 * @param p The memory
 */
void
xnvme_fs_dma_free(struct xnvme_fs_session *s, void *p);
/**
 * Registers the dma-buf 'fd', memory of another device such as a GPU, as a destination of direct
 * reads. The caller reserves a range of addresses of the dma-buf's size at 'vaddr', with an mmap
 * of PROT_NONE, and never reads or writes through it; xnvme_fs_submit_direct() takes an address in
 * that range for the corresponding byte of the dma-buf. Returns -ENOTSUP where xnvme fs was built
 * with an xNVMe that does not register dma-bufs. The fd may be closed after the call; unregister
 * before the exporter frees the memory.
 *
 * @param s The session
 * @param fd The dma-buf
 * @param vaddr The start of the range of addresses reserved for the dma-buf
 *
 * @return 0 on success, -ENOTSUP where the xNVMe built in registers no dma-bufs, or another
 * negative errno
 *
 * @errno
 */
int
xnvme_fs_register_dmabuf(struct xnvme_fs_session *s, int fd, void *vaddr);
/**
 * Ends the registration that xnvme_fs_register_dmabuf() made under 'vaddr'
 *
 * @param s The session
 * @param vaddr The start of the range of addresses, as given when registering
 *
 * @return 0 on success, -ENOTSUP where the xNVMe built in registers no dma-bufs, or another
 * negative errno
 *
 * @errno
 */
int
xnvme_fs_unregister_dmabuf(struct xnvme_fs_session *s, void *vaddr);
/**
 * Starts a read directly into 'dma', memory from xnvme_fs_dma_alloc() or of a registered dma-buf;
 * the CPU neither reads nor writes the bytes. The range must be whole blocks that the file has on
 * the drives, with no hole and no appends held in memory, else the request fails with -EINVAL.
 * Where the file ends inside the last block, the rest of that block is what the drive holds, which
 * need not be zeros.
 *
 * @param f The file
 * @param r The request, which is done or was never submitted
 * @param dma Where the blocks are read to, once the request is done
 * @param nbytes The number of bytes to read, a multiple of xnvme_fs_block_size()
 * @param offset Where in the file the read starts, a multiple of xnvme_fs_block_size()
 *
 * @return 0 on success, a negative errno on failure
 *
 * @errno
 */
int
xnvme_fs_submit_direct(struct xnvme_fs_file *f, struct xnvme_fs_req *r, void *dma, uint64_t nbytes,
		       uint64_t offset);
/**
 * Reaps completions without waiting, and says whether the request is done
 *
 * @param r The request
 * @param[out] done Set to 1 when the request is done, to 0 when not
 *
 * @return 0 on success, a negative errno where reaping failed, -EINVAL where 'done' is NULL
 *
 * @errno
 */
int
xnvme_fs_poll(struct xnvme_fs_req *r, int *done);
/**
 * Waits until the request is done, and returns the number of bytes moved, or a negative errno
 *
 * @param r The request
 *
 * @return The number of bytes moved, or a negative errno
 *
 * @errno
 */
ssize_t
xnvme_fs_wait(struct xnvme_fs_req *r);

#ifdef __cplusplus
}
#endif

#endif
