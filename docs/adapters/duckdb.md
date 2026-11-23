<!--
SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>

SPDX-License-Identifier: CC-BY-4.0
-->

# DuckDB

The extension `xnvme_fs` registers a DuckDB file system for the paths
`xnvme-fs://POOL/NAME`. A database, its write-ahead log and its temporary
files can all be stored on a pool:

```sql
LOAD xnvme_fs;
ATTACH 'xnvme-fs://pool/db.duckdb' AS db;
SET temp_directory = 'xnvme-fs://pool/tmp';
```

`XnvmeFsPath::Parse()` takes the pool's name from the first component after
`xnvme-fs://`. The rest of the path, with doubled and trailing slashes
removed, is the name in the pool. A path with no pool fails with an
`InvalidInputException`. A name that the pool does not allow, such as a name
with a `..` component, fails with the server's error.

## Loading the extension

`LoadInternal()` adds the extension's three settings and registers the file
system and the table function:

```{literalinclude} ../../adapters/src/xnvme_fs_extension.cpp
:language: c++
:start-at: config.AddExtensionOption(
:end-at: xnvme_fs::RegisterPoolFunctions(loader);
:dedent:
:tab-width: 4
```

| Setting | Type | Default |
|--|--|--|
| `xnvme_fs_socket_dir` | `VARCHAR` | `XNVME_FS_SOCKET_DIR`, else `/run/xnvme-fs` |
| `xnvme_fs_backend` | `VARCHAR` | `XNVME_FS_BACKEND`, else `upcie` |
| `xnvme_fs_homi_id` | `UINTEGER` | `XNVME_FS_HOMI_ID`, else 0 |

The extension checks `xnvme_fs_backend` when the setting is set, since the
extension connects to a pool only when the pool is first used.

## One session per pool

`GetSession()` keeps one session for each pool name in the process, in a
registry that every database of the process shares. `GetSession()` makes the
session from the settings when the pool is first used. It makes a new session
when the session's connection has failed, or when `xnvme_fs_socket_dir` is set
and differs from the socket directory of the session:

```{literalinclude} ../../adapters/src/pool_connection.cpp
:language: c++
:start-at: std::shared_ptr<::xnvme::fs::Session> GetSession(
:end-before: } // namespace xnvme_fs
```

`Raise()` converts a `std::system_error` of the C++ layer into one of
DuckDB's exceptions. `EINVAL`, `ENAMETOOLONG` and `ENOTSUP` become an
`InvalidInputException`, and every other errno becomes an `IOException`. The
exception's message is the C function's name and the message of
`xnvme_fs_errmsg()`.

## Files

`XnvmeFsFileSystem::OpenFile()` maps DuckDB's open flags to the flags of
`xnvme_fs_open()`. DuckDB has two flags that ask for `nullptr`, one when the
file exists and one when it does not. For either flag, `OpenFile()` calls
`xnvme_fs_stat()` before the code below and returns `nullptr` when the flag's
condition holds:

```{literalinclude} ../../adapters/src/xnvme_fs_filesystem.cpp
:language: c++
:start-at: uint32_t open_flags = 0;
:end-at: return make_uniq<XnvmeFsFileHandle>
:dedent:
:tab-width: 4
```

DuckDB's positional reads and writes are `xnvme_fs_pread()` and
`xnvme_fs_pwrite()` on the calling thread. A positional read that returns
fewer bytes than DuckDB asked for fails with an `IOException`, since DuckDB
reads only ranges that a file holds. For DuckDB's sequential reads and writes,
the handle keeps a position, and a file opened for appending writes at its
size. A file sync first waits for the calling thread's writes behind, which
the section on temporary files describes, and then calls `xnvme_fs_fsync()`:

```{literalinclude} ../../adapters/src/xnvme_fs_filesystem.cpp
:language: c++
:start-at: void XnvmeFsFileSystem::FileSync(
:end-before: void XnvmeFsFileSystem::Seek(
```

Directory operations map to `xnvme_fs_mkdir()`, `xnvme_fs_rmdir()` and
`Session::List()`. A move maps to `xnvme_fs_rename()` and works within one
pool only. A removal maps to `xnvme_fs_remove()`. The pool stores no times, so
the extension reports the modification time 0 for every file. Globs and
compressed files fail with a `NotImplementedException`.

## Temporary files written behind

The extension writes behind for DuckDB's temporary files, which are the files
whose names start with `duckdb_temp_`. A write behind is a write that returns
before the drives have completed it. `WriteBehind()` copies the bytes, starts
an asynchronous write with `xnvme_fs_submit()` on the calling thread, and
returns without waiting:

```{literalinclude} ../../adapters/src/write_behind.cpp
:language: c++
:start-at: void WriteBehind(const std::shared_ptr<::xnvme::fs::File> &file
:end-before: void Reap(bool wait) {
```

`Start()` submits the write and never throws. A write that could not be
started is marked as done with its errno, and the errno is reported when the
write is reaped.

```{literalinclude} ../../adapters/src/write_behind.cpp
:language: c++
:start-at: void Start(Behind &b) {
:end-before: //! Submits a write again
```

An open, and every positional read and write, first reaps the writes behind
that the calling thread started, with `xnvme_fs_poll()`. A sync, and a
truncate of a temporary file, wait for those writes. A write behind that
failed fails the operation that reaps it with an `IOException`. Only the
thread that submitted a write can poll it, so a thread that exits first waits
for its writes. A write that must be written again at that point is left for
the next thread that reaps.

DuckDB may reuse a block of a temporary file while the write that held the
block is in flight, and the drive may complete two writes to one range in
either order. The extension therefore copies a later write to a range that a
write in flight covers into the earlier write as well, and writes the earlier
write again once it is reaped. A read of a temporary file copies the bytes of
the writes in flight over the bytes it read from the drives (`Overlay`). A
truncate that would cut a write in flight does nothing, so that no extent the
write may be written to is leased to another file. The extents are freed when
DuckDB removes the file.

## Pool information

The table function `xnvme_fs_pool_info(POOL)` returns a row for each member
of the pool. The values come from `Session::Info()`, which calls
`xnvme_fs_info()` and `xnvme_fs_members()`:

| Column | Type | What |
|--|--|--|
| `pool` | `VARCHAR` | the pool's name |
| `member_index` | `BIGINT` | the member's place in the pool |
| `member` | `VARCHAR` | the member's name |
| `size_bytes` | `BIGINT` | the member's size |
| `extents` | `BIGINT` | the member's extents |
| `free_extents` | `BIGINT` | the member's extents that the server can lease |
| `layout_version` | `INTEGER` | the layout version of the superblocks |
| `extent_size` | `BIGINT` | the extent size in bytes |
| `generation` | `BIGINT` | the generation of the last table written |
| `files` | `BIGINT` | the files the server has, committed or not |

```sql
SELECT member, free_extents FROM xnvme_fs_pool_info('pool');
```
