<!--
SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>

SPDX-License-Identifier: CC-BY-4.0
-->

# xnvme fs

xnvme fs is a user-space file system on NVMe drives, built on xNVMe.

The drives of a file system form a pool. A pool is a set of members, and a
member is an NVMe namespace, a GPT partition of a namespace, or an md RAID 0
set of namespaces or partitions. The metadata of a pool is the names of its
files and the extents that store the bytes of each file. The metadata is
stored on the members and served by one process, `xnvme fs serve`, on a Unix
socket.

The file data does not pass through that server. A client process asks the
server which extents store a file, and then reads and writes those extents on
its own NVMe queues. The client reaches the drives in one of two ways:
through xNVMe's uPCIe backend, as a client of a HOMI server that has opened
the controllers, or through the Linux kernel's NVMe driver with io_uring.

The repository holds these parts:

* `libxnvme-fs`, a shared library with the C interface `libxnvme_fs.h`
* the program `xnvme-fs`, which xNVMe's front-end runs as `xnvme fs`, with the
  commands `mkfs`, `serve`, `info` and `cp`, and the helpers `mkfs.xnvme-fs`
  and `mount.xnvme-fs`
* the systemd unit `xnvme-fs@.service`, one instance per pool
* the DuckDB extension `xnvme_fs` and the RocksDB plugin `xnvme_fs`, which
  store the files of those engines in a pool

## The processes

Three kinds of process take part when a pool is used through uPCIe:

| Process | Role |
|--|--|
| `xnvme homi serve`, of xNVMe | Opens the controllers, which are bound to vfio-pci. It creates I/O queues for each of its clients and allocates DMA memory to them from one heap. |
| `xnvme fs serve` | A client of HOMI. It reads the superblocks and the table of the pool, allocates extents to files, writes the table, and answers the requests of the clients on the socket of the pool. Optionally it serves a read-only FUSE view of the pool. |
| A client: an engine with its adapter, `xnvme fs cp`, or any program that uses `libxnvme_fs.h` | A client of HOMI for file data, with one I/O queue per controller for each of its threads, and a client of `xnvme fs serve` for metadata. |

With the kernel's NVMe driver there is no HOMI server. `xnvme fs serve` and
the clients then open the block or character devices of the namespaces
directly.

## A write, from open to sync

`libxnvme-fs` takes these steps for a program that creates a file, writes to
it and syncs it:

1. `xnvme_fs_connect()` connects to the socket of the pool and sends a hello
   message. The server answers with the extent size, the offset of the data
   area and the names of the members. The library then opens every member.
2. `xnvme_fs_open()` asks the server to open the file. The server answers
   with a handle, the size of the file and the extents of the file.
3. For a write beyond the extents of the file, the library asks the server
   for a lease of more extents. The server allocates free extents, clears
   them with Write Zeroes commands, and appends them to the extent list of
   the file, which the server holds in memory.
4. The library writes the bytes to the extents on the queues of the calling
   thread.
5. `xnvme_fs_fsync()` flushes every member and then commits the size of the
   file to the server. The server writes the table of the pool to member 0
   and answers. The table names every committed file with its size and its
   extents.

[Concepts](concepts/pools.md) describes each part. [C API](api.md) lists the
interface, and [Tools](tools.md) lists the command lines.
[Adapters](adapters/index.md) shows how DuckDB and RocksDB use the interface.
[Getting started](getting-started.md) goes from one drive with a new
partition table to both engines storing their files on it.

```{toctree}
:maxdepth: 2
:caption: Overview

getting-started
```

```{toctree}
:maxdepth: 2
:caption: Concepts

concepts/pools
concepts/extents
concepts/leases
concepts/server
concepts/md-raid0
concepts/view
concepts/access
```

```{toctree}
:maxdepth: 2
:caption: C API

api
```

```{toctree}
:maxdepth: 2
:caption: Tools

tools
```

```{toctree}
:maxdepth: 2
:caption: Adapters

adapters/index
adapters/duckdb
adapters/rocksdb
```

```{toctree}
:maxdepth: 2
:caption: Inside xnvme fs

design
reproducing
```
