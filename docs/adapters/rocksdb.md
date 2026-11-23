<!--
SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>

SPDX-License-Identifier: CC-BY-4.0
-->

# RocksDB

The plugin is a RocksDB `FileSystem`, `XnvmeFsFileSystem` in
`adapters/rocksdb/xnvme_fs_rocksdb.cc`, which RocksDB's build links in as
`plugin/xnvme_fs`. The plugin registers the URI `xnvme-fs://POOL`, so a
RocksDB program that takes a file system by URI can store its files on a pool:

```
db_bench --fs_uri=xnvme-fs://pool --db=/bench
```

RocksDB's paths name files in the pool: `/bench/000012.sst` is the pool's
`bench/000012.sst`. `Name()` removes leading, doubled and trailing slashes and
`.` components.

## Registering the file system

For a plugin that RocksDB's build links in, RocksDB calls the function that
the plugin's `xnvme_fs.mk` names, `RegisterXnvmeFs()`. `RegisterXnvmeFs()`
adds a factory for the URIs `xnvme-fs://...`. The factory makes a file system
for the pool that the URI names:

```{literalinclude} ../../adapters/rocksdb/xnvme_fs_rocksdb.cc
:language: c++
:start-at: extern "C" int RegisterXnvmeFs(
```

## The session

The file system makes its session at the first call that needs one, so
constructing a file system sends no request to the pool's server. It makes a
new session when the connection has failed. The socket directory and the
HOMI id come from `XNVME_FS_SOCKET_DIR` and `XNVME_FS_HOMI_ID`. The plugin
passes an empty backend, for which `libxnvme-fs` uses `XNVME_FS_BACKEND`, else
`upcie`:

```{literalinclude} ../../adapters/rocksdb/xnvme_fs_rocksdb.cc
:language: c++
:start-at: std::shared_ptr<::xnvme::fs::Session> Session() {
:end-before: std::string pool;
:dedent:
:tab-width: 4
```

`Guard()` converts a `std::system_error` of the C++ layer into an `IOStatus`
of RocksDB:

| errno | `IOStatus` |
|--|--|
| `ENOENT` | `PathNotFound` |
| `ENOSPC` | `NoSpace` |
| `ENOTSUP` | `NotSupported` |
| `EINVAL`, `ENAMETOOLONG` | `InvalidArgument` |
| every other errno | `IOError` |

## Reading

`RandomAccessFile::Read()` calls `xnvme_fs_pread()`. `MultiRead()` starts
every read of RocksDB's batch with `xnvme_fs_submit()` before it waits for any
of them. All reads of the batch are therefore in flight at once, on the
calling thread's queues on the members that hold the ranges:

```{literalinclude} ../../adapters/rocksdb/xnvme_fs_rocksdb.cc
:language: c++
:start-at: IOStatus MultiRead(FSReadRequest *reqs
:end-before: IOStatus ReadAsync(
:dedent:
:tab-width: 4
```

`ReadAsync()` starts one read and returns the request to RocksDB as the I/O
handle. `Poll()` reaps the reads with `xnvme_fs_poll()` and calls RocksDB's
callback for each read that is done. When a pass over the handles reaps
nothing, `Poll()` waits for the first read that is not done, with
`xnvme_fs_wait()`:

```{literalinclude} ../../adapters/rocksdb/xnvme_fs_rocksdb.cc
:language: c++
:start-at: IOStatus Poll(std::vector<void *> &io_handles
:end-before: //! Requests on a device cannot be withdrawn
:dedent:
:tab-width: 4
```

A command submitted to a drive cannot be withdrawn, so `AbortIO()` waits for
the reads and does not call RocksDB's callback. A pool cannot be mapped into
memory. For a file opened with `use_mmap_reads`, `MappedFile` reads the whole
file into memory when the file is opened, and serves reads from that copy.

## Writing

`NewWritableFile()` opens the file with
`XNVME_FS_O_CREATE | XNVME_FS_O_TRUNC`, and `WritableFile::Append()` writes at
the file's size with `xnvme_fs_pwrite()`. RocksDB's `Flush()` does nothing.
`Sync()` calls `xnvme_fs_fsync()`, which commits the file:

```{literalinclude} ../../adapters/rocksdb/xnvme_fs_rocksdb.cc
:language: c++
:start-at: // Like a page cache, XNVME_FS_APPEND_BUFFER
:end-before: IOStatus Close(const IOOptions &, IODebugContext *) override {
:dedent:
:tab-width: 4
```

With `XNVME_FS_APPEND_BUFFER` set, appends smaller than one command stay in
memory after RocksDB's `Flush()`, until `Sync()` or the file's close. A
directory's `Fsync()` does nothing, since a commit, a rename and a removal
each write the pool's table before they return.

## The LOCK file

RocksDB locks a database with its `LOCK` file. The server allows one process
at a time to have a given file open. The plugin therefore keeps `LOCK` open
while it holds the lock, and another process's open of `LOCK` fails with
`EBUSY`. A second lock in the same process would open the same file again
without an error. `LockFile()` keeps a set of the names locked in the process
and fails a lock on a name that is in the set:

```{literalinclude} ../../adapters/rocksdb/xnvme_fs_rocksdb.cc
:language: c++
:start-at: IOStatus LockFile(
:end-before: IOStatus UnlockFile(
:dedent:
:tab-width: 4
```

## The rest of the file system

`GetChildren()` calls `Session::List()` and reports a missing directory as
`NotFound`, as RocksDB's POSIX file system does. The other calls map to the C
API as follows:

| RocksDB | C API |
|--|--|
| `CreateDir()`, `CreateDirIfMissing()` | `xnvme_fs_mkdir()` |
| `DeleteDir()` | `xnvme_fs_rmdir()` |
| `DeleteFile()` | `xnvme_fs_remove()` |
| `RenameFile()` | `xnvme_fs_rename()` |
| `FileExists()`, `GetFileSize()`, `IsDirectory()` | `xnvme_fs_stat()` |

The pool stores no times, so the plugin reports the modification time 0 for
every file. `GetTestDirectory()` returns `TEST_TMPDIR`, else `/tmp`, as
RocksDB's POSIX file system does.
