<!--
SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>

SPDX-License-Identifier: CC-BY-4.0
-->

# Leases and commits

`xnvme fs serve` keeps the pool's metadata in memory and is the only process
that changes the metadata. A client keeps a copy of the map of each file it
has open. The client extends the map by leasing extents from the server, and
makes the file's size and map durable by committing them. The server writes
the committed state to member 0 as the pool's table.

## Opening a file

A client opens a file with the request `OPEN`, which names the file and
carries the flags create, truncate and exclusive. The server answers with a
handle, the file's size as the server knows it, and the file's map. `OPEN`
fails with `ENOENT` for a file that does not exist unless the create flag is
given, with `EEXIST` for a file that exists when both create and exclusive are
given, and with `EISDIR` for a directory.

The connection that opens a file is the file's owner until the connection
closes its last handle on the file. While a file has an owner, an `OPEN`,
`REMOVE` or `RENAME` of the file from any other connection fails with `EBUSY`,
and so does an `RMDIR` of a directory that contains the file. One file is
therefore read and written by one client process at a time.

Within a process, `libxnvme-fs` keeps one open file for each name, and every
`xnvme_fs_open()` of that name returns a handle to that open file. The server
sees one handle, and the handles in the process share one size and one map. An
`xnvme_fs_open()` of a name that the process has open fails with `EEXIST` when
given `XNVME_FS_O_CREATE` and `XNVME_FS_O_EXCL`, and truncates the shared file
when given `XNVME_FS_O_TRUNC`.

## Leasing extents

A write that ends past the last extent of the file's map sends the request
`LEASE` for the missing extents, at most 4096 extents per request
(`MAX_LEASE`), and appends the extents in the answer to the map. The server
allocates the extents as [Extents and striping](extents.md) describes, clears
them, and appends them to its own copy of the map. The extents belong to the
file until a truncate, a close or a removal removes them from the map. A lease
does not expire.

The server does not check what a client writes. Nothing prevents a client from
writing to other extents; a correct client writes only to the extents of the
files it has open.

## The three sizes of a file

A file has three sizes:

* The size in the process that has the file open. `xnvme_fs_size()` returns
  this size, which includes the appends buffered in memory.
* The size that the server last received, in the file's last `COMMIT`,
  `TRUNCATE` or `CLOSE`. The creation of a file and an `OPEN` with the
  truncate flag set this size to 0. Other processes see this size through
  `xnvme_fs_stat()`, `xnvme_fs_list()`, the index and the view.
* The durable size, which the table stores. A `COMMIT` sets the durable size,
  and a `TRUNCATE` to a smaller size reduces it.

`xnvme_fs_stat()` and `xnvme_fs_list()` return the first size for a file that
the calling process has open, and the second size for any other file.

## Sync and commit

`xnvme_fs_fsync()` first writes the appends that the file buffers in memory.
When the file has changed since its last sync, `xnvme_fs_fsync()` then sends
the NVMe Flush command to every drive of every member of the pool and waits
for the flushes to complete. `xnvme_fs_fsync()` sends `COMMIT` with the file's
size only after that, so the data is on the drives before the server writes a
table that refers to the data.

The server handles `COMMIT` by setting the file's size and durable size to the
committed size. When the file has a name, the server also marks the file as a
file in the table and writes the table. For a file that was removed while
open, the server only sets the sizes. A file enters the table at its first
commit, or when it is renamed.

## The table

The table holds the pool's namespace and the files' maps as of one commit. The
table contains every directory made with `MKDIR`, and every file in the table
with its durable size and all the extents the file has at the time of the
write, including extents leased past the durable size. The server writes the
whole table at every change that it commits. The server writes the table of
generation `g` to table slot `g % 2` of member 0, flushes member 0, and only
then records `g` as the generation written. The other slot still holds the
previous table.

A table slot starts with a header of 64 bytes, little-endian:

| Offset | Size | Field |
|--|--|--|
| 0 | 8 | The magic, `XNVMEMAP` |
| 8 | 4 | The layout version, 2 |
| 12 | 4 | The CRC-32 of the header, with this field zero, and of the payload |
| 16 | 16 | The pool's UUID |
| 32 | 8 | The generation |
| 40 | 8 | The length of the payload in bytes |

The payload follows the header. The payload is a 32-bit count of entries, and
then for each entry its kind (8 bits, 1 for a file and 2 for a directory), its
name (a 32-bit length and the bytes), its size (64 bits) and its extents (a
32-bit count and one 64-bit address per extent). The server writes the slot in
whole 4 KiB blocks. When the table does not fit in a slot, the request that
caused the write fails with `ENOSPC`.

When the server starts, it reads both slots. A slot is valid when its magic,
version, UUID and checksum are valid, and the server uses the valid slot with
the higher generation. A crash during a table write leaves a table whose
checksum fails, so the server uses the other slot. The start fails with
`EINVAL` when neither slot is valid. When a table write fails with an error
other than `ENOSPC`, the server exits with status 2, because the metadata in
its memory and the table on the drive then differ in what is committed.

The following requests write the table: a `COMMIT` of a file that has a name,
an `MKDIR` of a name not made with `MKDIR` before, an `RMDIR`, a `REMOVE` of a
file in the table, and a `RENAME` to another name. `TRUNCATE` and `CLOSE` do
not write the table, and the server does not write the table when it stops.

## Truncate

`xnvme_fs_ftruncate()` first writes the appends that the file buffers in
memory. When the new size is smaller than the old size and lies inside an
extent of the file, not on an extent boundary, `xnvme_fs_ftruncate()` writes
zeros from the new size to the end of that extent or to the old size,
whichever comes first. A later write past the new end therefore leaves zeros
between the new end and the range written. `xnvme_fs_ftruncate()` then sends
`TRUNCATE` and removes the extents past the new size from the client's map.

The server removes the same extents from its copy of the map, sets the size,
and reduces the durable size to the new size when the durable size is larger.
The table on the drive keeps the old size and the old extents until the next
table write.

## Freeing extents

When the server removes an extent from a file that is in the table, the table
on the drive may still contain the extent. The server keeps the extent
allocated until it has written a table that does not contain the extent. An
extent removed from a file that is not in the table is free immediately,
unless a read lease delays the release; [Read leases](#read-leases) describes
that case.

## Close

When a process closes its last handle on a file, `libxnvme-fs` writes the
appends that the file buffers in memory and sends `CLOSE` with the file's
size. The server stores the size. When the connection has no handle left on
the file, the server removes the extents past that size from the file's map,
and the file has no owner. A close does not commit: the durable size and the
table remain as the last commit left them.

## When a connection ends

When a connection ends, the server closes each handle of the connection as
`CLOSE` does, with the size that the server last received. A file whose client
exits without closing it therefore keeps the size of its creation or of its
last commit, truncate or close, and the server removes the extents past that
size from the file's map.

A file that was never committed remains in the server's namespace after its
client's connection ends, with the size that the server last received, and
another client can open and read the file. The file is not in the table, so
the file is lost when the server stops or crashes.

## Removal and rename

`REMOVE` removes the file's name from the namespace and writes the table when
the file was in the table. The server frees the file's extents when no handle
is open on the file. A handle that a process still has on the removed file
remains usable.

`RENAME` gives a file a new name. When another file has the new name, the
server removes that file as `REMOVE` does, and the rename fails with `EBUSY`
when another connection has that file open. The server marks the renamed file
as a file in the table, with its durable size, and writes the table. A rename
of a directory fails with `ENOTSUP`.

## Directories and names

A directory exists when `MKDIR` made it, or when the pool has a file or a
directory whose name starts with the directory's name followed by `/`. A
client can therefore create the file `a/b` without an `MKDIR` of `a`. Only
directories made with `MKDIR` are in the table. `RMDIR` removes a directory
and every file and directory below it, and fails with `EBUSY` when another
connection has one of those files open.

A name in the pool is 1 to 255 bytes and consists of components separated by
`/`. A component cannot be empty, `.` or `..`, so a name has no leading,
trailing or doubled `/`. The empty name is the top directory of the pool.
`OPEN`, `MKDIR` and `RMDIR`, and `RENAME` for the new name, fail with
`ENAMETOOLONG` for a name that is empty or longer than 255 bytes, and with
`EINVAL` for an invalid component.

## Read leases

The request `INDEX` is for a reader that is not one of the database engines
using the pool: a program that reads the files the engines write and submits
its own I/O to the drives. The answer to `INDEX` is an index of the pool. The
index contains the following:

* every file's name, the size that the server last received, and the extents
  that this size needs
* every directory made with `MKDIR`
* each member's name, its md chunk size, and its drives, each drive with the
  byte offset at which the member starts on the drive

The answer also carries a lease and the version of the index.

While the reader holds the lease, the server allocates no extent that the
index may contain to another file. An extent that the server removes from a
file while any read lease is held stays allocated. The server frees the extent
at a `RELEASE` or at the end of a connection, once no remaining lease was
taken at the index version at which the extent was removed, or at an earlier
version.

A reader sends `WATCH` with the lease, on the connection that took the lease.
The server answers `WATCH` once a change to names, sizes or extents has made
the index stale. The changes are an `OPEN` that creates or truncates a file, a
`CLOSE`, `COMMIT`, `TRUNCATE`, `RMDIR` or `REMOVE`, an `MKDIR` of a new
directory, and a `RENAME` to another name. The reader then requests a new
index. `RELEASE` ends the lease. When an engine overwrites bytes in place, the
reader reads the new bytes and the index does not become stale: a read lease
protects the allocation of extents, not their contents.

The C interface has no functions for read leases. `Client::MakeIndex()`,
`Client::Watch()` and `Client::Release()` in `lib/protocol.hpp` send the three
requests, and `tests/index.cpp` uses them.
