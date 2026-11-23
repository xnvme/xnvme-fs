<!--
SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>

SPDX-License-Identifier: CC-BY-4.0
-->

# Configuration

The adapters and `xnvme-fs-bench` read the first two from the environment and pass them to `xnvme_fs_connect()`,
whose arguments they are; `xnvme fs` reads them for its own defaults. libxnvme-fs reads the others from the
environment of every process that uses a pool: `xnvme fs serve`, the engines, and programs on the C interface. The
DuckDB extension also takes `XNVME_FS_BACKEND` as the default of its setting.

| Variable | What |
|--|--|
| `XNVME_FS_HOMI_ID` | the HOMI server holding the pool's controllers |
| `XNVME_FS_SOCKET_DIR` | where the pools' sockets are; `/run/xnvme-fs` when unset |
| `XNVME_FS_BACKEND` | how members are reached: `upcie`, the default, `io_uring`, `io_uring_direct` or `io_uring_cmd` |
| `XNVME_FS_APPEND_BUFFER` | set to anything but 0, files hold appends in memory until a sync, up to one command; faster, but a process that dies loses what it buffered |
| `XNVME_FS_POLL_MODE` | `paced`, the default, first sleeps for half of what the thread's waits on requests of its size took, then polls; `spin` polls the queue for the whole wait, holding the core. Any other value fails |
| `XNVME_FS_STATS` | a file the library appends a line to each second for each member, from samples of its commands in flight taken every millisecond: the samples, those with a command in flight and their depths summed, for all commands and for reads, and the reads, writes and bytes so far |
| `XNVME_FS_TRACE` | a file every library operation is appended to as a line: the time, process and thread, the operation, the file, and its offset and size; for counting what an engine asks of the pool |
