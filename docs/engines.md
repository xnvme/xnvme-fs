<!--
SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>

SPDX-License-Identifier: CC-BY-4.0
-->

# Engines

Every engine reaches a pool the same way. Metadata, opening and creating
files, leases and commits, goes over the socket of `xnvme fs serve`; data goes
from the engine's own threads to the members, each thread on queues of its own.
Both adapters use the C interface of xnvme fs, `libxnvme_fs.h`, through
`include/yace/xnvme_fs.hpp`, the C++ layer yace generates from it, and
`include/xnvme_fs.hpp`, which adds what the two share.

## DuckDB

The `xnvme_fs` extension registers a file system for paths
`xnvme-fs://POOL/NAME`. The database, its write-ahead log and its temporary
files can all live on a pool:

    LOAD xnvme_fs;
    ATTACH 'xnvme-fs://pool/db.duckdb' AS db;
    SET temp_directory = 'xnvme-fs://pool/tmp';

Temporary files are written behind, several writes in flight per thread, and
read back from the writes still in flight. The extension's settings, each
defaulting to its environment variable:

| Setting | Default | What |
|--|--|--|
| `xnvme_fs_socket_dir` | `XNVME_FS_SOCKET_DIR`, else `/run/xnvme-fs` | where pools' sockets are |
| `xnvme_fs_backend` | `XNVME_FS_BACKEND`, else `upcie` | how members are reached: `upcie`, `io_uring`, `io_uring_direct` or `io_uring_cmd` |
| `xnvme_fs_homi_id` | `XNVME_FS_HOMI_ID` | the HOMI server holding the members |

`xnvme_fs_pool_info('pool')` returns a row for each member: the pool, the
member's index and name, its size, extents and free extents, the layout
version, the extent size, the table's generation and the number of files.

## RocksDB

The plugin is a `FileSystem` that RocksDB's build links in as
`plugin/xnvme_fs`. It registers the URI `xnvme-fs://POOL`, so any RocksDB
program takes it as its file system:

    db_bench --fs_uri=xnvme-fs://pool --db=/bench

It reads `XNVME_FS_SOCKET_DIR` and `XNVME_FS_HOMI_ID`. Reads RocksDB issues
together, through `MultiRead` and `ReadAsync`, are all in flight at once.
