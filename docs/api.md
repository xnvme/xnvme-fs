<!--
SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>

SPDX-License-Identifier: CC-BY-4.0
-->

# C API

A program uses xnvme fs through the C header `libxnvme_fs.h` and the shared
library `libxnvme-fs`. The pkg-config file `xnvme-fs.pc` gives the compiler
and linker flags:

```
cc prog.c $(pkg-config --cflags --libs xnvme-fs)
```

The library is written in C++17. The linker script `lib/libxnvme-fs.map`
exports the 29 functions of the header and no other symbol, so a C program
links the library like any C library. The sections below list the functions
with their declarations, grouped by the object the functions act on.

## Conventions

A function returns 0, a count, or a negative errno. There are three
exceptions. `xnvme_fs_dma_alloc()` returns a pointer, or `NULL` with `errno`
set. `xnvme_fs_block_size()` returns the block size. `xnvme_fs_disconnect()`,
`xnvme_fs_req_free()` and `xnvme_fs_dma_free()` return nothing.

A function that fails writes a message to a buffer of the calling thread, and
`xnvme_fs_errmsg()` returns that buffer. The message carries detail that the
errno does not, such as the name, the member or the NVMe status, for example
`xnvme fs: no file 'tpch/lineitem.parquet'`. When the server answers a
request with an error, the message is the server's message.

A function returns `-EINVAL` when a string argument or an output argument is
`NULL`, unless the description of the function gives `NULL` a meaning. The
library does not check the session, file and request pointers.

Several threads may use one session. The library sends one request at a time
on the session's connection to the server, and modifies the state of a file
while holding the file's mutex. Each thread submits its commands on its own
queues, so reads and writes from different threads run concurrently. The
thread that submitted an asynchronous request is the thread that polls it and
waits for it, because the request's commands are on that thread's queues.

## An example

`xnvme fs cp` is a client written against this header. The function `In()`
copies a local file into the pool. `In()` opens the file with
`XNVME_FS_O_CREATE | XNVME_FS_O_TRUNC`, writes it, syncs it and closes it, and
reports the first failure with the message that `xnvme_fs_errmsg()` returned:

```{literalinclude} ../tools/cp.c
:language: c
:start-at: In(struct xnvme_fs_session
:end-before: static int
:prepend: static int
```

`failed_fs()` records the errno and copies the message of
`xnvme_fs_errmsg()`. `xnvme_fs_cp_main()` creates the session with
`xnvme_fs_connect()`:

```{literalinclude} ../tools/cp.c
:language: c
:start-at: err = xnvme_fs_connect(
:end-at: homi ? (uint32_t)strtoul
```

## Types and constants

```c
struct xnvme_fs_session;
struct xnvme_fs_file;
struct xnvme_fs_req;
```

The three handles are opaque. `struct xnvme_fs_session` is the connection of
a process to the server of one pool, and `xnvme_fs_connect()` creates it.
`struct xnvme_fs_file` is a file of the pool that the process has open, and
`xnvme_fs_open()` creates it. `struct xnvme_fs_req` is an asynchronous read or
write, and `xnvme_fs_req_new()` creates it.

```c
#define XNVME_FS_O_CREATE 1
#define XNVME_FS_O_TRUNC  2
#define XNVME_FS_O_EXCL   4
```

These are the flags of `xnvme_fs_open()`. `XNVME_FS_O_CREATE` creates the
file when it does not exist. `XNVME_FS_O_TRUNC` truncates the file to size 0.
`XNVME_FS_O_EXCL` together with `XNVME_FS_O_CREATE` fails the open when the
file exists.

```c
#define XNVME_FS_KIND_FILE      1
#define XNVME_FS_KIND_DIRECTORY 2
```

These are the values of `kind` in `struct xnvme_fs_stat` and
`struct xnvme_fs_entry`. A `kind` of 0 means that the name does not exist.

```c
#define XNVME_FS_NAME_MAX 255
```

`XNVME_FS_NAME_MAX` is the maximum length of the name of a file or directory
in the pool, and of the path of a member that `xnvme_fs_members()` returns.
The length is in bytes and does not count the terminating NUL.

```c
struct xnvme_fs_stat {
	uint64_t size;
	uint8_t kind;
};
```

`xnvme_fs_stat()` fills this struct. `kind` is `XNVME_FS_KIND_FILE`,
`XNVME_FS_KIND_DIRECTORY` or 0. `size` is the size of a file in bytes, and 0
for a directory.

```c
struct xnvme_fs_info {
	uint32_t layout_version;
	uint32_t nmembers;
	uint64_t extent_size;
	uint64_t generation;
	uint64_t files;
};
```

`xnvme_fs_info()` fills this struct with the state of the pool in its
server: the layout version of the pool's superblocks, the number of members,
the extent size in bytes, the generation of the last table the server wrote,
and the number of files, both committed and uncommitted.

```c
struct xnvme_fs_entry {
	uint64_t size;
	uint8_t kind;
	uint8_t rsvd[7];
	char name[XNVME_FS_NAME_MAX + 1];
};
```

`struct xnvme_fs_entry` is one entry of a directory and is 272 bytes. `size`
is the size of a file, and 0 for a directory. `kind` is the kind of the
entry. `name` is the NUL-terminated name of the entry within the directory.

```c
struct xnvme_fs_member {
	uint64_t size;
	uint64_t extents;
	uint64_t free_extents;
	char path[XNVME_FS_NAME_MAX + 1];
};
```

`struct xnvme_fs_member` is one member of the pool. `size` is the size of the
member in bytes, `extents` is the number of its extents, and `free_extents`
is the number of those extents that are free. `path` is the NUL-terminated
name of the member as the superblock lists it.

## Errors

### xnvme_fs_errmsg()

```c
const char *xnvme_fs_errmsg(void);
```

Returns the message of the last call that failed on the calling thread, or
`""` when no call has failed on the thread. The pointer refers to a
thread-local buffer of 512 bytes and is the same on every call. The pointer
stays valid until the thread exits. The buffer keeps the message until the
next call fails on the thread. A call that succeeds does not change the
buffer.

## Sessions

### xnvme_fs_connect()

```c
int xnvme_fs_connect(const char *pool, const char *socket_dir, const char *backend,
                     uint32_t homi_id, struct xnvme_fs_session **s);
```

Connects to the server of `pool` at the socket `<socket_dir>/<pool>.sock`,
sends the request `HELLO` with the protocol version and the name of the pool,
and opens every member of the pool with `backend`. With the backend `upcie`,
the members are opened through the HOMI server with the id `homi_id`. When
`backend` is `NULL` or empty, the backend is the value of `XNVME_FS_BACKEND`,
or `upcie` when that variable is unset or empty. The call sets `*s` to the
session, or to `NULL` when the call fails. The call reads
`XNVME_FS_APPEND_BUFFER` and `XNVME_FS_STATS` for the session;
[Tools](tools.md#environment) describes the variables.

The call fails with these errors:

| Error | Condition |
|--|--|
| the errno of `connect(2)`, such as `-ENOENT` or `-ECONNREFUSED` | no server listens on the socket |
| `-ENAMETOOLONG` | the path of the socket does not fit a Unix socket address |
| `-EPROTO` | the server uses another protocol version |
| `-ENOENT` | the server serves another pool |
| `-ENOTSUP` | `backend` is not a known backend |
| `-EINVAL` | the backend is `upcie` and `homi_id` is 0 |
| the error of opening the member | a member cannot be opened |

### xnvme_fs_disconnect()

```c
void xnvme_fs_disconnect(struct xnvme_fs_session *s);
```

Frees `s`. Every request submitted on a file of the session must be done
before the call. While a file of the session is still open, the connection to
the server stays open; the library closes the connection when the last such
file is closed.

### xnvme_fs_info()

```c
int xnvme_fs_info(struct xnvme_fs_session *s, struct xnvme_fs_info *info);
```

Fills `info` with the state of the pool in its server. The library gets the
state with the request `INFO`.

### xnvme_fs_members()

```c
int xnvme_fs_members(struct xnvme_fs_session *s, uint64_t offset,
                     struct xnvme_fs_member *members, uint32_t max, uint32_t *count,
                     uint64_t *remaining);
```

Fills `members` with up to `max` members of the pool, starting at index
`offset`, in the order of the pool. The call sets `*count` to the number of
members written and `*remaining` to the number of members after those. With
`max` 0, `*remaining` is the number of members from `offset` on, and
`members` may be `NULL`. The members of a pool do not change while the pool
is served, so `xnvme_fs_members()` has no `version` argument as
`xnvme_fs_list()` has. `free_extents` is the number of extents of the member
that are not allocated to a file and that the server can lease.

### xnvme_fs_block_size()

```c
uint32_t xnvme_fs_block_size(struct xnvme_fs_session *s);
```

Returns the block size of the pool in bytes, which is the largest block size
of the pool's members. The offset and the length of
`xnvme_fs_submit_direct()` must be multiples of the block size.

## Names

A name is 1 to 255 bytes long and consists of components separated by `/`.
No component is empty, `.` or `..`. `xnvme_fs_list()` takes `""` as the name
of the top directory. `xnvme_fs_open()`, `xnvme_fs_mkdir()` and
`xnvme_fs_rmdir()` validate their name, and `xnvme_fs_rename()` validates
`to`. These calls fail with `-ENAMETOOLONG` for a name that is empty or
longer than 255 bytes, and with `-EINVAL` for a name with a component that is
empty, `.` or `..`.

### xnvme_fs_stat()

```c
int xnvme_fs_stat(struct xnvme_fs_session *s, const char *name, struct xnvme_fs_stat *st);
```

Fills `st` for `name`. When `name` does not exist, the call also returns 0,
and sets `st->kind` to 0. When the session `s` has the file open, `st->size`
is the size of the file in the session. Otherwise `st->size` is the size that
the server recorded last, which is the size at the file's creation, at an
open with `XNVME_FS_O_TRUNC`, or at the file's last commit, truncate or
close.

### xnvme_fs_list()

```c
int xnvme_fs_list(struct xnvme_fs_session *s, const char *directory, uint64_t offset,
                  struct xnvme_fs_entry *entries, uint32_t max, uint32_t *count,
                  uint64_t *remaining, uint64_t *version);
```

Fills `entries` with up to `max` entries of `directory`, starting at index
`offset`, sorted by name. The call sets `*count` to the number of entries
written and `*remaining` to the number of entries after those. The server
returns at most 4096 entries for one request, so `*count` may be less than
`max` while `*remaining` is not 0. With `max` 0, `*remaining` is the number
of entries from `offset` on, and `entries` may be `NULL`. The size of a file
in an entry is the size that `xnvme_fs_stat()` returns for the file.

A caller reads a directory in pages by calling `xnvme_fs_list()` again with
`offset` advanced by `*count`, until `*remaining` is 0. A name that is added,
removed or renamed between two calls shifts the offsets. The caller detects
such a change with `*version`. The caller sets `*version` to 0 before the
first call, and the call sets `*version` to the version of the names in the
pool. A later call that is given this version fails with `-ESTALE` when the
names have changed since, and the caller starts over from `offset` 0 and
`*version` 0. The call fails with `-ENOENT` when `directory` is not a
directory.

`list_all()` in `tests/capi.c` reads a directory in pages:

```{literalinclude} ../tests/capi.c
:language: c
:start-at: list_all(struct xnvme_fs_session
:end-before: static pthread_key_t
:prepend: static int
```

### xnvme_fs_mkdir()

```c
int xnvme_fs_mkdir(struct xnvme_fs_session *s, const char *name);
```

Creates the directory `name`. The call also returns 0 when the directory
exists. The call fails with `-EEXIST` when `name` is a file. A file can be
created under a directory that no call to `xnvme_fs_mkdir()` created; the
pool's table then has no entry for that directory.
[Leases and commits](concepts/leases.md#directories-and-names) describes
directories.

### xnvme_fs_rmdir()

```c
int xnvme_fs_rmdir(struct xnvme_fs_session *s, const char *name);
```

Removes the directory `name` and every file and directory under it. The call
fails with `-EBUSY` when another session has one of those files open, with
`-ENOTDIR` when `name` is a file, and with `-ENOENT` when the directory does
not exist.

### xnvme_fs_remove()

```c
int xnvme_fs_remove(struct xnvme_fs_session *s, const char *name);
```

Removes the file `name`. The call fails with `-EBUSY` when another session
has the file open, with `-EISDIR` when `name` is a directory, and with
`-ENOENT` when the file does not exist. A file that the session `s` has open
stays open, and the process can still read and write it. The server frees
the extents of the file when the process closes it.

### xnvme_fs_rename()

```c
int xnvme_fs_rename(struct xnvme_fs_session *s, const char *from, const char *to);
```

Renames the file `from` to `to`, and removes an existing file named `to`. A
file that the session `s` has open under the name `from` has the name `to`
after the call. Renaming a file to its own name changes nothing. The
call fails with `-ENOTSUP` when `from` is a directory, with `-ENOENT` when
the file `from` does not exist, with `-EISDIR` when `to` is a directory, and
with `-EBUSY` when another session has `from` or `to` open. The server writes
the renamed file to the pool's table with the size of the file's last commit,
also when the file was not in the table before.

## Files

### xnvme_fs_open()

```c
int xnvme_fs_open(struct xnvme_fs_session *s, const char *name, uint32_t flags,
                  struct xnvme_fs_file **f);
```

Opens the file `name` with the `XNVME_FS_O_*` bits in `flags`, and sets `*f`
to the file, or to `NULL` when the call fails. The call fails with `-ENOENT`
when the file does not exist and `XNVME_FS_O_CREATE` is not set, with
`-EEXIST` when the file exists and both `XNVME_FS_O_CREATE` and
`XNVME_FS_O_EXCL` are set, with `-EISDIR` when `name` is a directory, and
with `-EBUSY` when another session has the file open.

All opens of one name in a session share one file object, with one size, one
extent map and one handle at the server. When the session already has the
name open, `xnvme_fs_open()` sends no request to the server. The call then
fails with `-EEXIST` for `XNVME_FS_O_CREATE | XNVME_FS_O_EXCL`, and truncates
the shared file for `XNVME_FS_O_TRUNC`.

### xnvme_fs_close()

```c
int xnvme_fs_close(struct xnvme_fs_file *f);
```

Writes the appends that the file holds in memory, and frees `f` whether the
write succeeds or fails. The call returns 0, or the negative errno of the
write. Every request submitted on `f` must be done before the call. When the
session closes its last open of the name, the library sends the request
`CLOSE` with the size of the file, and the server frees the extents leased
past that size. `xnvme_fs_close()` does not commit the file, so a file that
was never committed is not in the pool's table after the close.

### xnvme_fs_size()

```c
int64_t xnvme_fs_size(struct xnvme_fs_file *f);
```

Returns the size of the file in bytes in its session, including the appends
held in memory.

### xnvme_fs_pread()

```c
ssize_t xnvme_fs_pread(struct xnvme_fs_file *f, void *buf, uint64_t nbytes, uint64_t offset);
```

Reads up to `nbytes` bytes of the file, starting at `offset`, into `buf`,
and waits for the read to complete. The call returns the number of bytes
read. That number is less than `nbytes` when the read reaches the end of the
file, and 0 when `offset` is at or past the end. For a range of the file that
has no extent, the library fills `buf` with zeros and sends no command to a
drive.

### xnvme_fs_pwrite()

```c
ssize_t xnvme_fs_pwrite(struct xnvme_fs_file *f, const void *buf, uint64_t nbytes,
                        uint64_t offset);
```

Writes the `nbytes` bytes of `buf` to the file at `offset`, and waits for
the write to complete. The call leases the extents that the write needs from
the server, and returns `nbytes`. A lease fails with `-ENOSPC` when the pool
has too few free extents.

When `XNVME_FS_APPEND_BUFFER` is set, the library keeps an append that is
shorter than one command in memory and sends no write command for it.
`xnvme_fs_fsync()`, `xnvme_fs_ftruncate()` or `xnvme_fs_close()` writes the
appends held in memory.

The call returns when the drives have completed the write commands. The call
does not flush the drives and does not write the size of the file to the
pool's table; `xnvme_fs_fsync()` does both.

### xnvme_fs_fsync()

```c
int xnvme_fs_fsync(struct xnvme_fs_file *f);
```

Makes the data and the size of the file durable. The call writes the appends
held in memory. When the file has changed since its last sync, the call then
flushes every member and sends the request `COMMIT` with the size of the
file, and the server writes the pool's table. Apart from
`xnvme_fs_rename()`, `xnvme_fs_fsync()` is the only call that adds a file to
the table.

### xnvme_fs_ftruncate()

```c
int xnvme_fs_ftruncate(struct xnvme_fs_file *f, uint64_t size);
```

Writes the appends held in memory, and then sets the size of the file to
`size` bytes. When `size` is smaller than the size of the file, the call
discards the bytes past `size`, and the server frees the extents past `size`.
When `size` is larger, the bytes between the old size and `size` read as
zeros. The new size is durable after the next `xnvme_fs_fsync()`.

## Asynchronous requests

A program allocates a request once with `xnvme_fs_req_new()` and can submit
the request any number of times. `xnvme_fs_submit()` or
`xnvme_fs_submit_direct()` starts the request. The request is done when
`xnvme_fs_poll()` sets `*done` to 1 or when `xnvme_fs_wait()` returns. A
request must be done before it is submitted again, before it is freed, before
`xnvme_fs_close()` of its file, and before `xnvme_fs_disconnect()` of the
session. `tests/capi.c` submits reads of two ranges, polls the first request,
and waits for both requests:

```{literalinclude} ../tests/capi.c
:language: c
:start-at: CHECK(xnvme_fs_submit(f, a, 0, ina, MIB, 0) == 0);
:end-at: CHECK(xnvme_fs_wait(b) == MIB);
:dedent:
:tab-width: 4
```

### xnvme_fs_req_new()

```c
int xnvme_fs_req_new(struct xnvme_fs_req **r);
```

Allocates a request and sets `*r` to the request, or to `NULL` when the call
fails. The call fails with `-ENOMEM` when the allocation fails, and with
`-EINVAL` when `r` is `NULL`.

### xnvme_fs_req_free()

```c
void xnvme_fs_req_free(struct xnvme_fs_req *r);
```

Frees `r`. The request must be done or must never have been submitted. The
call does nothing when `r` is `NULL`.

### xnvme_fs_submit()

```c
int xnvme_fs_submit(struct xnvme_fs_file *f, struct xnvme_fs_req *r, int write, void *buf,
                    uint64_t nbytes, uint64_t offset);
```

Starts a read or a write of `nbytes` bytes at `offset`, and returns without
waiting for the commands to complete. `write` 0 starts a read, and any other
value of `write` starts a write. The `buf` of a read must stay valid until
the request is done. The library copies the bytes of a write before the call
returns, so the caller may reuse `buf` when the call has returned.

The library copies an append that does not start and end on block boundaries
into a buffer of the file. The library writes that buffer to the drives
before the call returns, unless `XNVME_FS_APPEND_BUFFER` is set and the
append is shorter than one command. The call leases the extents that a write
needs before it returns. When a request cannot be started, the request is
done, and `xnvme_fs_wait()` returns the error.

### xnvme_fs_poll()

```c
int xnvme_fs_poll(struct xnvme_fs_req *r, int *done);
```

Reaps completions on the queues of the calling thread without waiting, and
sets `*done` to 1 when the request is done and to 0 when it is not. The call
returns `-EINVAL` when `done` is `NULL`, and a negative errno when reaping
fails.

### xnvme_fs_wait()

```c
ssize_t xnvme_fs_wait(struct xnvme_fs_req *r);
```

Waits for the request in the mode that `XNVME_FS_POLL_MODE` selects, and
returns the result of the request. The result is the number of bytes read or
written, or a negative errno. A read that reaches the end of the file returns
fewer bytes than were asked for. After a negative errno, `xnvme_fs_errmsg()`
returns the message of the request. When a command completes with an NVMe
error status, the request fails with `-EIO`, and the message gives the status
code type and the status code. A call to `xnvme_fs_wait()` on a request that
is done returns the same result again.

## DMA memory and direct reads

### xnvme_fs_dma_alloc()

```c
void *xnvme_fs_dma_alloc(struct xnvme_fs_session *s, uint64_t nbytes);
```

Allocates `nbytes` of DMA memory, which is memory that the drives read and
write directly as the data buffer of a command. The call returns the memory,
or `NULL` with `errno` set; `errno` is `ENOMEM` when the allocation fails.
`xnvme_fs_dma_free()` frees the memory. The library allocates the memory
through the first member of the pool. With the backend `upcie`, the memory
comes from the heap of the HOMI server.

### xnvme_fs_dma_free()

```c
void xnvme_fs_dma_free(struct xnvme_fs_session *s, void *p);
```

Frees `p`, memory that `xnvme_fs_dma_alloc()` returned for `s`.

### xnvme_fs_register_dmabuf()

```c
int xnvme_fs_register_dmabuf(struct xnvme_fs_session *s, int fd, void *vaddr);
```

Registers the dma-buf `fd` as a destination of `xnvme_fs_submit_direct()`. A
dma-buf is memory of another device, such as a GPU. After the call, the
address range that starts at `vaddr` and has the size of the dma-buf
identifies the dma-buf as the argument `dma` of `xnvme_fs_submit_direct()`.
The caller reserves that address range, for example with an `mmap()` of
`PROT_NONE`, and never reads or writes it. The call registers the dma-buf
with xNVMe through the first member of the pool. The caller may close `fd`
after the call. The caller calls `xnvme_fs_unregister_dmabuf()` before the
exporter of the dma-buf frees the memory. The call returns `-ENOTSUP` when
xnvme fs was built with an xNVMe that does not register dma-bufs.

### xnvme_fs_unregister_dmabuf()

```c
int xnvme_fs_unregister_dmabuf(struct xnvme_fs_session *s, void *vaddr);
```

Removes the registration that `xnvme_fs_register_dmabuf()` made for the
address range at `vaddr`. The call returns `-ENOTSUP` under the same
condition as `xnvme_fs_register_dmabuf()`.

### xnvme_fs_submit_direct()

```c
int xnvme_fs_submit_direct(struct xnvme_fs_file *f, struct xnvme_fs_req *r, void *dma,
                           uint64_t nbytes, uint64_t offset);
```

Starts a read of `nbytes` bytes at `offset` in which the drives transfer the
data directly into `dma`, and the library does not copy the bytes on the
CPU. `dma` is memory from `xnvme_fs_dma_alloc()` or the address range of a
registered dma-buf. `offset` and `nbytes` must be multiples of
`xnvme_fs_block_size()`. The range must consist of blocks of the file that
are stored on the drives, so it must not include a range without an extent
or appends held in memory. A request that does not meet these conditions
fails with `-EINVAL`, and `xnvme_fs_wait()` returns that error. When the file
does not end on a block boundary, the part of the last block past the end of
the file contains the bytes that the drive stores there. The result of the
request is `nbytes`.

## What the C API leaves out

The C API has no function for the read leases of the protocol, which are the
requests `INDEX`, `WATCH` and `RELEASE`.
[Leases and commits](concepts/leases.md#read-leases) describes read leases.
`Client::MakeIndex()`, `Client::Watch()` and `Client::Release()` in
`lib/protocol.hpp` send the three requests.
