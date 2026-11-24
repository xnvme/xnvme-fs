<!--
SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>

SPDX-License-Identifier: CC-BY-4.0
-->

# xnvme fs design

This page describes why xnvme fs is built the way it is. The pages under
Concepts specify the parts in detail: [pools](concepts/pools.md),
[extents](concepts/extents.md), [leases and commits](concepts/leases.md), the
[server and its protocol](concepts/server.md), the [view](concepts/view.md),
[access to the drives](concepts/access.md) and
[md RAID 0 members](concepts/md-raid0.md). The last section lists what the
design names and the code does not implement.

## Goals

1. DuckDB's database, write-ahead log and temporary (spill) files are stored
   on raw NVMe through DuckDB's `FileSystem` interface, with no change to
   DuckDB.
2. The drives run at their bandwidth: each thread has a deep asynchronous
   queue on each drive, spill writes are written behind, and the submission
   path takes no lock.
3. A pool has several drives from the start. Extents are striped across the
   drives, so that bandwidth grows with the number of drives.
4. A block can be read into memory other than host memory. A read can
   transfer a block from a drive to an accelerator's memory peer to peer, so
   that a scan operator on a GPU needs no new storage layer.
5. Correctness comes before speed: the write-ahead log is durable, the layout
   is consistent after a crash, and a test suite runs in CI without NVMe
   hardware.

## Non-goals

The following are not goals: a general-purpose file system, Parquet and CSV
files on the drives (they stay on a regular file system), engines other than
DuckDB and RocksDB, a writable FUSE view, and drives other than NVMe drives.

## Architecture: AiSIO with HOMI

xnvme fs demonstrates the AiSIO architecture. In AiSIO, several independent
processes share NVMe controllers. One process owns each controller, and every
process submits commands on I/O queues of its own.

xnvme fs builds against a commit of the branch `wip` of safl/xnvme, whose
uPCIe backend has that control plane. `xnvme homi serve BDF --be upcie --homi-id N` owns a
controller: the device, its DMA memory and its admin queue. The HOMI server
serves clients on `/tmp/xnvme-homi-N.sock`. A client opens the same device
with `--be upcie --homi-id N` and gets queues of its own. The open fails with
`ENOENT` when no HOMI server serves the device.

Three kinds of process take part:

| Process | Unit | Role |
|---|---|---|
| HOMI | `xnvme-homi@<id>` (xNVMe) | owns the controllers through vfio and the IOMMU; creates I/O queues for its clients |
| `xnvme fs serve` | `xnvme-fs@<pool>` | HOMI client; the only process that changes the pool's metadata: superblock, extent maps, leases; serves the metadata socket and a read-only FUSE view |
| DuckDB or RocksDB with its adapter (the DuckDB extension `xnvme_fs`, the RocksDB plugin `xnvme_fs`) | the application | HOMI client for data, with I/O queues of its own per thread; client of the socket of `xnvme fs serve` for metadata |

### Why not the kernel's file systems

AiSIO can interoperate with a kernel file system. That takes four parts:
HOMI, qublk (a block device for the kernel), XFS, and xal (a library that
resolves files to LBAs, as FIEMAP does).
xnvme fs takes the other approach. xnvme fs defines the on-drive layout
itself, and no kernel driver handles its I/O. `xnvme fs serve` shows the
pool's files through a read-only FUSE mount, which is a view for management
and not an overlay on an existing file system. xnvme fs needs neither qublk
nor xal.

### Clients are database instances

The clients are DuckDB and RocksDB. The two engines have opposite access
patterns. DuckDB does analytics, with large block reads and bursts of spill
writes. RocksDB is an LSM store, with log appends, many immutable files and
background compaction. Supporting both shows that the design is not fitted to
one engine.

Each engine uses xnvme fs through its own pluggable file layer, and is not
modified: DuckDB through its file system extension interface, RocksDB through
a `FileSystem` plugin that RocksDB loads by URI. Each instance has its own
files and extents. `xnvme fs serve` divides a pool between the instances with
leases, and allows one process at a time to have a given file open. RocksDB's
`LOCK` file relies on that rule to lock a database across processes. An
engine that requires `mmap` or the kernel page cache cannot use xnvme fs.

### The metadata socket

`xnvme fs serve` listens on a Unix socket, `/run/xnvme-fs/<pool>.sock`.
Neither engine's file interface has extents, only files and byte offsets;
extents belong to xnvme fs. The socket carries a small file service that both
adapters use. Its operations are hello, open, close, lease, commit, truncate,
stat, list, mkdir, rmdir, remove, rename and info, and for read leases index,
watch and release.

The adapter runs inside the engine's process. Through `libxnvme-fs`, the
adapter opens and closes files on the socket, leases whole extents (64 MiB
each by default), maps the engine's file offsets to its leased extents
itself, commits a file's size and extent map when the engine syncs the file,
and removes files. Reads and writes of file data do not use the socket.

RocksDB uses more of the service than DuckDB does: renames (for its `CURRENT`
file), directory listings and syncs, a `LOCK` file, and the creation and
removal of many files as compaction runs.

`xnvme fs serve` writes the table of maps on its own queues. Each write is
copy-on-write to one of two alternating slots, and carries a generation
number.

### The FUSE view

`xnvme fs serve --view /mnt/aisio/<pool>` mounts the pool read-only and lists
its files: the databases, their write-ahead logs and their spill files. Each
file has the size that `xnvme fs serve` last received from the file's client,
at a commit, a truncate or a close.

A read in the view goes through the queues of `xnvme fs serve`, while DuckDB
does I/O on the same controller. The view opens files for direct I/O, so the
kernel caches no pages of a file that an engine changes.

`xnvme fs serve` handles the FUSE protocol on `/dev/fuse` itself, with ufuse
(github.com/safl/ufuse, the meson wrap `subprojects/ufuse.wrap` at v0.4.0).
ufuse is a header-only library that depends only on the kernel's
`<linux/fuse.h>`. The descriptor of `/dev/fuse` is one more descriptor in the
poll loop of `xnvme fs serve`, so a read through the view delays metadata
requests for the duration of the read. One read is at most 128 KiB. Mounting
the view needs `CAP_SYS_ADMIN`.

### Readers outside the engines

A process other than an engine can read the files the engines write. An
example is xal indexing a pool. Such a reader requests an index on the
socket. The index holds every file's size and extents, and where each
member's drives start on their namespaces. The server grants the index under
a read lease. The reader addresses the drives itself through HOMI.

While a read lease is held, `xnvme fs serve` allocates no extent that the
index names to another file. An extent that a truncate or a removal frees
stays allocated until every read lease taken at or before that moment is
released, or the connection that holds the lease closes.

A reader sends a watch request for its lease on a connection of its own.
`xnvme fs serve` answers the watch when a change to names, sizes or extents
makes the index stale. The reader then discards its index and requests a new
one.

Bytes that an engine overwrites in place change under the reader without an
answer to the watch, as they would under a reader of a kernel file system.
The read lease protects extents, not their contents.

### Failure

When DuckDB exits or crashes, its connection closes. `xnvme fs serve` then
closes DuckDB's files, and frees the extents leased past the size it last
received for each file. Files that DuckDB never synced, its spill files among
them, stay in the server's namespace with their extents until they are
removed or the server stops. The table does not name these files, so a
restarted server does not have them and their extents are free.

When `xnvme fs serve` exits or crashes, the table on the drive is
authoritative. A restarted server reads the table, and DuckDB's next open
goes through the restarted server.

### Trust

A queue from HOMI can address the whole namespace. A client is trusted to
write only to the extents it has leased. Restricting a client to its extents
would need one namespace per client or support from the IOMMU, and is outside
the design.

## A model for the kernel

xnvme fs also models what a kernel file system would need to offer for AiSIO
to do I/O on its files directly:

* regions assigned to clients
* extents granted under a lease, which stay in place while the lease is held
* allocation ahead of writes
* maps committed at a sync
* leases that the file system can reclaim or recall

A process gets that information today from FIEMAP, which is advisory, or by
parsing on-disk metadata, as xal does for XFS. XFS's layout leases for pNFS
block layouts are a related mechanism. This design has not been compared with
them.

## Names

"xnvme fs" names the parts that do not depend on an engine: `xnvme fs serve`,
the on-drive layout, the socket protocol and the FUSE view. The binding of an
engine is an adapter: the DuckDB extension `xnvme_fs` and the RocksDB
`FileSystem` plugin `xnvme_fs`.

"AiSIO" names the architecture that xnvme fs demonstrates. Once the mapping
from a file's bytes to blocks on the drives is known, any process can do I/O
on those blocks directly, and can inspect the files through an ordinary
mounted view.

## Where xnvme fs plugs into DuckDB

DuckDB routes the database, the write-ahead log, temporary files and external
files through its `VirtualFileSystem`. `RegisterSubSystem` adds a file system
to the `VirtualFileSystem` (`common/virtual_file_system.cpp`). Requests are
synchronous `pread` and `pwrite` calls on the calling thread
(`common/local_file_system.cpp`). Database blocks are 256 KiB and spill
blocks are 32 to 256 KiB. DuckDB does no read-ahead for spill files. The file
references are to DuckDB v2.0.0.

xnvme fs registers for the prefix `xnvme-fs://`. `ATTACH
'xnvme-fs://pool0/tpch.db'` stores a database on the pool `pool0`, and `SET
temp_directory = 'xnvme-fs://pool0/tmp'` stores the spill files there.

## Layout

A *pool* is one or more NVMe namespaces, its *members*. Each member starts
with a superblock that names the pool, its members, its layout version (2),
its extent size (default 64 MiB) and its geometry. Two table slots follow the
superblock, and the data area follows the slots. The data area is divided
into extents. Only the slots of member 0 are used. Every member reserves the
space for the slots, so all members have the same data offset.

The *table* holds the pool's namespace and extent maps: every directory, and
every file with its durable size and its extents in file-offset order.
`xnvme fs serve` writes the whole table, copy-on-write, to the slot that the
table's generation selects, and flushes the member. The table's header
carries the pool's UUID and a CRC-32. A torn write fails the checksum, and a
table that an earlier pool left on the same drive has another UUID, so
`xnvme fs serve` reads neither back. On start, `xnvme fs serve` takes the
valid slot with the higher generation.

### Which files are in the table

xnvme fs has one kind of file. DuckDB's database, its write-ahead log and its
spill files differ in when DuckDB syncs them, and the syncs alone decide
which files are in the table:

| File | Synced by DuckDB | In the table |
|---|---|---|
| database | at checkpoint | from its first sync, with the size of its last |
| write-ahead log | at every commit | the same |
| spill file | never | never; stays in the server's namespace until removed or the server stops |

A file enters the table at its first commit, or when a rename gives it a new
name. A file leaves the table when it is removed. The size of a file in the
table is the size at its last commit, not at its last write.

A file that was never committed remains after the client that wrote it has
gone, and the next client can read it, as a closed file remains in the
kernel's page cache. The file is lost when `xnvme fs serve` stops, as a crash
of a kernel loses a file that was never synced.

When a truncate or a removal takes extents from a file in the table, the
extents become free only after `xnvme fs serve` has written a table without
them.

### Zeros past the end of a file

`xnvme fs serve` clears an extent with the Write Zeroes command before it
leases the extent to a file. A truncate that cuts an extent writes zeros from
the new size to the end of that extent, or to the old size if that comes
first. A range of a file that nothing wrote therefore reads as zeros, and no
file returns bytes that an earlier file left on the drive.

### Appends

An open file keeps the block that holds its end in memory. An append that
starts or ends inside a block, such as a log record, is written from that
copy as whole blocks, without first reading the block from the drive. The
bytes past the end of the file in that block are zeros, as the previous
section describes.

The file writes such an append before the call returns, while holding the
file's lock, so two appends to one block reach the drive in order. The block
and the appends after it are kept in DMA memory of the size of one command,
called the stage. An append is copied once, into the stage, and written from
the stage.

With `XNVME_FS_APPEND_BUFFER` set, a file keeps appends smaller than one
command in the stage and serves reads of them from the stage. The file writes
the staged appends as one command at a sync, a truncate, the file's close, a
write that reaches the block holding the file's end, or when the stage is
full. An append of one command or more is written when it is made. Buffering
issues fewer commands. A process that exits or crashes loses the appends in
the stage, and a failure to write them is reported by the handle's close.

## I/O

Each thread that does I/O gets an xNVMe queue of its own on each drive of
each member, created at the thread's first use of the drive. Submissions
therefore take no lock.

DuckDB's calls stay synchronous. A read or a write of a database file or the
write-ahead log waits on the calling thread, with every command of the
request in flight at once over the members. Writes to spill files are written
behind: the adapter copies the bytes, submits the write asynchronously on the
calling thread and reaps it later. A read of a spill file copies the bytes of
the writes still in flight over the bytes that the drives returned.

### The number of queues limits the number of threads

In the kernel, blk-mq maps per-CPU software queues onto the hardware queues
that the controller grants, however many those are, so an application does
not depend on that number. xnvme fs gives each thread a queue of its own on
each drive. The number of I/O queues that a controller grants therefore
limits the total number of threads of all clients of the drive, the thread of
`xnvme fs serve` included. Controllers differ in how many I/O queues they
grant. HOMI also limits the number of queue pairs of one client over all its
controllers.

A thread that cannot create a queue fails with an error that names the
drive, gives the number of queues the process already has there, and says to
limit the number of threads. Two changes would remove the limit at a cost per
command: threads sharing a queue behind a lock, which is a software queue in
user space, or HOMI multiplexing clients onto fewer hardware queues. Neither
is implemented.

### Backends

xnvme fs is designed for xNVMe's uPCIe backend through HOMI. The drives are
bound to vfio-pci and owned by HOMI. DuckDB's threads and `xnvme fs serve`
submit commands on queues of their own as HOMI clients, and no kernel driver
is in the data path.

The same library also reaches the drives through the kernel's nvme driver,
selected with `Access.backend` or `XNVME_FS_BACKEND`. The three kernel
backends exist for comparison, and each differs from the previous one in one
respect:

| Backend | How it reaches the drive |
|---|---|
| `io_uring` | reads and writes the namespace's block device through the page cache |
| `io_uring_direct` | reads and writes the block device without the page cache |
| `io_uring_cmd` | passes the NVMe commands that xnvme fs builds for HOMI through the namespace's character device |

With `io_uring_cmd`, the difference from uPCIe is only which component owns
the queues.

On the kernel backends, xnvme fs waits for completions as it does on HOMI, by
paced polling of its queues, so the backend is the only difference between
runs. The reads and writes of `io_uring` cannot express Write Zeroes, so
xnvme fs writes zeros instead, and a sync is an `fsync` of the device.

md sets remain usable on the kernel backends. mdadm creates and inspects a
set on the block devices, and xnvme fs maps the set through the character
devices. The kernel's array must not be assembled for writing at the same
time.

### Waiting for completions

`Wait` polls without interrupts, and paces the polling. A thread keeps a
moving average of how long its waits took, per class of request size. A
request that is not done at the first poll sleeps for half of the average of
its class before the thread polls again, as the kernel's hybrid polling
sleeps before it polls. A thread that reads therefore does not occupy its
core for the whole wait on the drive. Flushes, and the writes of a stage,
wait the same way with averages of their own. `XNVME_FS_POLL_MODE=spin` makes
every wait poll without sleeping, which uses more CPU time per request.

Polling on the kernel backends does not use what the kernel offers there. The
kernel handles the drive's interrupt and can wake a waiting thread, through
an eventfd on the ring or through io_uring's own wait for completions.

## Partitions, mkfs and mount

A pool member is a controller that HOMI owns, such as `0000:84:00.0`, or one
partition of the controller's GPT, such as `0000:84:00.0/p2`. xnvme fs
follows the conventions of Linux file systems. The drive is partitioned with
`sgdisk`, `fdisk` or `parted` while the kernel's nvme driver has the drive.
The xnvme fs partition has the type `a04faa88-135a-4457-b8f6-db8bbaf344b8`,
and `lsblk` shows it beside the other partitions.

Once HOMI owns the controller, xnvme fs reads the GPT itself and checks both
of the GPT's checksums. xnvme fs fails with an error for a partition of any
other type, so a mistyped partition number does not format a partition that
holds another file system. xnvme fs adds the partition's start to the LBA of
every command.

`mkfs.xnvme-fs` and `mount.xnvme-fs` are the program `xnvme-fs` installed
under the names that `mkfs(8)` and `mount(8)` look for. Under those names the
program takes the command lines that `mkfs(8)` and `mount(8)` pass:

```
mkfs.xnvme-fs -L pool 0000:84:00.0/p2
mount.xnvme-fs 0000:84:00.0/p2 /mnt/pool       # or: mount -t xnvme-fs ...
umount /mnt/pool
```

`mount.xnvme-fs` reads the pool's name and its other members from the
superblock of the member it is given. It starts a server for the pool with
the read-only view at the mount point, and returns once the view is mounted.
The server logs to `/run/xnvme-fs/POOL.log` and stops when the view is
unmounted.

xnvme fs does not change the other partitions of the drive. The kernel cannot
reach them while HOMI owns the controller, so the controller cannot be the
one that holds the system's root file system. The same applies to any device
that HOMI serves, local or behind NVMe/TCP or NVMe/RDMA: xnvme fs opens the
device it is given.

## Stripe sets

A member may also be an md RAID 0 set. The member is named by the set's
drives joined by `+`, as in `0000:82:00.0+0000:83:00.0`. Each drive is a
controller or a partition of the type Linux RAID. The set is created and
inspected with the kernel's tools while the kernel's nvme driver has the
drives, and is given to HOMI once mdadm has stopped the set:

```
mdadm --create /dev/md0 --level=0 --raid-devices=2 /dev/nvme0n1 /dev/nvme1n1
mdadm --stop /dev/md0
mkfs.xnvme-fs -L pool 0000:82:00.0+0000:83:00.0
```

xnvme fs reads md's superblock, format 1.2, from each drive. It checks the
superblock's checksum and that all drives belong to one set, and orders the
drives by their roles. It maps chunk c of the array to chunk c / N of drive
c % N, as the kernel's md driver does. Each thread has a queue of its own on
each drive, so a request that spans chunks has commands in flight on every
drive it covers at once.

xnvme fs fails with an error for a set that it cannot map exactly as md does:
another RAID level, a reshape in progress, drives that differ in size after
rounding to whole chunks, and any other md feature. With HOMI stopped, the
kernel assembles the set again and reads the pool's bytes where xnvme fs
wrote them.

The extent remains the unit of allocation, over members that may each be a
stripe set. A set spreads every file over all its drives. Several sets in one
pool keep placement per set, so a file's extents on one member lie on that
member's drives only. That property is what would let one accelerator, with
the drives under its PCIe switch, read only the extents on its own drives.

The kernel does not offer the composite mapping in one place. On XFS over md,
FIEMAP returns offsets in the array. Turning the offsets into drives and
blocks also needs md's geometry, and both XFS and md may move blocks.

## Small appends and the page cache

xnvme fs does direct I/O: every read and every write goes to the drive unless
the append buffer is enabled. An engine on a kernel file system uses the page
cache by default, and the page cache hides the drive's latency from the
engine. An engine that relies on the page cache does not cache that data
itself. On xnvme fs the choice is explicit: the file layer buffers appends
when asked to, and otherwise does not.

The engines append to their logs in records that can be much smaller than a
block. A kernel file system copies such an append into its page cache and
returns. The drive receives the data at the next sync or when the kernel
writes it back. xnvme fs has no page cache, so each append is a write to the
drive: whole blocks from the block kept in memory, but still one command and
its completion. A kernel file system also serves reads from the page cache.
When a database fits in the host's memory, those reads need no command, while
every read on xnvme fs that the engine's own cache does not serve is a command
to the drive.

Small appends also amplify writes. Without the block kept in memory, an
append of a record smaller than a block is a read-modify-write: one block
read and one block written for the record. The block kept in memory removes
the read but not the rewrite. Each record rewrites the whole 4 KiB block that
holds it, so a block is written once per record until it is full. Buffering
until a sync writes each block once.

For writes, xnvme fs offers the one part of a page cache that a log needs.
With `XNVME_FS_APPEND_BUFFER`, a file keeps its appends in memory and writes
them at a sync, a truncate, the file's close, a write that reaches the block
holding the file's end, or once they fill one command. Reads of the buffered
appends are served from memory. Writes below the block holding the file's end
go to the drive directly. An append of one command or more is written when it
is made. The buffer has the size of one command, so that writing the buffer
out is a single write.

The append buffer keeps the engine's durability at a sync, as the page cache
does. It differs from the page cache in one respect: a process that exits or
crashes loses what it buffered, where the kernel would still write the pages
back. That is why the buffer is off by default. The append buffer caches
nothing for reads. An engine's own cache, RocksDB's block cache or DuckDB's
buffer pool, serves those.

## Memory and peer-to-peer

The C API has three calls for memory other than the caller's.
`xnvme_fs_dma_alloc()` returns DMA memory from HOMI's heap.
`xnvme_fs_register_dmabuf()` registers another device's memory, given as a
dma-buf. `xnvme_fs_submit_direct()` reads whole blocks of a file into either
kind of memory without the CPU accessing the bytes. Every other read and
write goes through the DMA buffers of the thread's queue and the caller's
host memory. DuckDB's own calls always use host buffers.

An accelerator's buffer comes from the accelerator's driver as a dma-buf of
device memory in the card's BAR. The card stays in the iommufd context of the
driver's device server. The client passes the dma-buf to HOMI. HOMI maps the
dma-buf with `IOMMU_IOAS_MAP_FILE` into the one IOAS that its controllers
share, so a drive's DMA to the buffer's IOVA reaches the device memory. HOMI
removes the mapping when the client unregisters the dma-buf or exits. The
client must unregister before the driver frees the buffer, since the driver
cannot revoke a dma-buf it has exported.

`xnvme_fs_register_dmabuf()` returns `-ENOTSUP` when xnvme fs was built with
an xNVMe that does not register dma-bufs. The commit of xNVMe that xnvme fs
builds against registers them, with `xnvme_buf_register_dmabuf()`.

## The library and the programs

The library under `lib/` does not depend on an engine and has everything an
adapter needs: a session with the `xnvme fs serve` of a pool, the members
that the session opened, and files that are shared by every handle a process
has on one name.

The interface is asynchronous at its core. `File::Submit` starts a request, a
read or a write of a range of a file. `File::Submit` splits the range at
extent boundaries and starts every piece on the queue of its member, so a
range that spans members has commands in flight on all of them, and then
returns. The thread that submitted the request reaps it with `Poll`, which
does not wait, or with `Wait`. The library has no background thread for I/O,
and the queues take no lock. `File::Read` and `File::Write` are `Submit`
followed by `Wait`. `libxnvme_fs.h` offers the same interface to C, as
POSIX-like calls that return a negative errno.

A member is driven with NVM read, write and flush commands in whole blocks. A
write that covers a block in part reads the block, merges the bytes and
writes the block back. A thread's queue on a drive is created at first use
and terminated when the thread exits. A queue holds 32 commands of at most
256 KiB, each with a buffer of DMA memory, so a request larger than one
command has all of its commands in flight at once.

`xnvme fs` is the program `xnvme-fs`, which the meson project at the top of
the repository builds. `xnvme fs mkfs --pool NAME --member MEMBER...
--homi-id N` formats a pool. `xnvme fs serve` serves the pool on
`/run/xnvme-fs/NAME.sock`, or on the path that `--socket` names. `xnvme fs
info` prints the pool's superblock and table.

`xnvme fs serve` and `xnvme fs mkfs` lock each drive of each member through a
file named for the drive under `/run/lock`, so two processes cannot format or
serve one controller at the same time. `xnvme fs serve` handles one request
at a time, table writes included. It exits when a table write fails for a
reason other than a full table slot, since its memory and the drive then
disagree and the drive is authoritative.

`xnvme-fs-bench` drives the library as the engines do, synchronously or with
a chosen number of requests in flight. With `--verify`, it writes each
sector's own file offset into the sector and checks the offset on reads.

### Tracing

With `XNVME_FS_TRACE` naming a file, the library appends one line to the file
for every operation an engine requests, through either adapter: open, read,
write, an asynchronous read or write submitted, sync, truncate, size, close,
stat, list, mkdir, rmdir, remove and rename, and the lease and commit
requests that those operations cause on the socket. A line holds the time,
the process and thread, the operation, the name, an offset or size, and the
number of bytes.

## The adapters

[Adapters](adapters/index.md) describes both adapters with their code. This
section gives the design decisions.

### DuckDB

The extension finds the socket through the setting `xnvme_fs_socket_dir`,
which defaults to `XNVME_FS_SOCKET_DIR` and then to `/run/xnvme-fs`. The
extension opens the members that `xnvme fs serve` names, through uPCIe by
default. A member is a controller named by its PCI address and owned by
`xnvme homi serve`. Every process that uses the pool, `xnvme fs serve`,
`xnvme fs mkfs` and each DuckDB, opens the controller as a client of that
HOMI server, with `--homi-id` or the setting `xnvme_fs_homi_id` (default
`XNVME_FS_HOMI_ID`).

DuckDB's storage layer opens files at points where the settings are not
available, so the extension keeps one session per pool per process and reuses
it. The table function `xnvme_fs_pool_info(name)` reports a pool's members,
extents and generation.

### RocksDB

The RocksDB plugin, in `adapters/rocksdb`, implements RocksDB's `FileSystem`
on the same library. RocksDB selects it by URI as `xnvme-fs://<pool>`
(`db_bench --fs_uri=xnvme-fs://pool0`), and RocksDB's paths name files in the
pool.

The plugin is built into RocksDB, which is pinned at v11.8.0 in
`adapters/third_party/rocksdb`. `make rocksdb` links the directory into
RocksDB's source tree as `plugin/xnvme_fs`. The plugin's `CMakeLists.txt`
names the source, the include path and the libraries that pkg-config gives
for `libxnvme-fs`, and its `xnvme_fs.mk` names the registration function.

`MultiRead` has every read of a batch in flight at once. `ReadAsync` and
`Poll` pass the library's requests to RocksDB. A directory's `Fsync` does
nothing, since a file's commit, a rename and a removal each reach the drive
before they return. `LockFile` keeps the lock file open, which `xnvme fs
serve` allows one client to do at a time, and fails a second lock within the
process, as RocksDB's POSIX file system does.

A pool cannot be mapped into memory. RocksDB opens a file with
`use_mmap_reads` only after it has finished writing the file, and its
PlainTable format needs such files. The plugin reads such a file into memory
when it is opened and serves reads from that copy.

## Testing and CI

CI needs no NVMe hardware. A QEMU guest on GitHub's runners has emulated NVMe
controllers, bound to vfio-pci and owned by HOMI, and the extension, the
layout and the I/O path all run on them.

The cijoe workflow `xnvme-fs-guest`, which CI runs after the build, boots a
QEMU guest with an IOMMU and two emulated NVMe controllers bound to vfio-pci,
and starts `xnvme homi serve` on them. It then runs each suite as a step, on
a pool of both controllers:

* DuckDB's sqllogictests with the database and `temp_directory` on xnvme fs,
  beside `xnvme fs serve`. One of them runs TPC-H at scale factor 1 against
  DuckDB's reference answers, with a `memory_limit` small enough that DuckDB
  spills.
* DuckDB's storage tests
* the test of the writes behind
* `xnvme-fs-bench` with `--verify`, as a correctness check
* RocksDB's file system conformance tests, its basic database tests and
  `db_bench`, with `TEST_FS_URI=xnvme-fs://ci`
* a DuckDB and a RocksDB instance on one pool at the same time
* the trace: the step `test_trace` traces DuckDB through TPC-H at scale
  factor 1 and RocksDB through `db_bench`, and prints for each engine the
  operations it used, how many times, and the sizes of its reads and writes

The workflow then binds the controllers to the kernel's nvme driver and runs
the test of the writes behind, the benchmark, RocksDB's tests and the two
engines on one pool again, through each kernel backend. The same targets run
on any host with `XNVME_FS_MEMBER` and `XNVME_FS_HOMI_ID` set. They overwrite
the controllers.

The guest suites also run without GitHub.
`cijoe/workflows/xnvme-fs-build.yaml` with `configs/local.toml` builds the
tree: the step `xnvme_fs_build` copies the checkouts of xNVMe and of this
repository to the machine it targets and builds xNVMe, xnvme fs, the
extension, `xnvme-fs-bench` and a Release `db_bench` there.
`xnvme-fs-guest.yaml` then runs every suite in the QEMU guest as CI does. The
guest needs KVM, which under WSL2 needs nested virtualization.

The suites of the file system itself are `cijoe/tests/test_fs.py`. They cover
a member's I/O, the C interface, the command line, the FUSE view, partitions,
md sets, and a crash test that kills a writer, and in some rounds the server,
at random points while records are synced. `cijoe/tests/test_fs.py` needs
four controllers and root in the guest. No workflow and no CI job runs it.

## Build

The DuckDB extension is built with DuckDB's extension template and CI tools,
against a pinned DuckDB release, with xNVMe and its uPCIe backend as a
dependency. Building DuckDB takes long. CI builds it, and the cijoe workflow
`xnvme-fs-build` builds it on a host of the user's choice.

The pins are git submodules under `adapters`: `duckdb` at v2.0.0,
`extension-ci-tools` on its branch `v2.0-cyanoptera`, and
`third_party/rocksdb`. The adapters find `libxnvme-fs` with pkg-config, as
the meson project at the top of the repository installs it into the prefix of
an xNVMe with uPCIe and HOMI. When `cmake -S adapters` is run on its own, not
from DuckDB's build, it builds only the target that needs no DuckDB,
`xnvme-fs-bench`.

## What is not implemented

The design names the following, and the code does not have them:

* read-ahead for scans, larger stripe-aligned spill writes, and asynchronous
  checkpoint writes
* a wait mode that sleeps until the kernel wakes the thread on the kernel
  backends, which would be selected like `XNVME_FS_POLL_MODE` and not by
  backend
* queues shared between threads, or multiplexed by HOMI, to lift the limit on
  the number of threads
* a scan process on an accelerator as a further HOMI client on the same
  controllers. `xnvme_fs_register_dmabuf()` and `xnvme_fs_submit_direct()`
  are the interface such a process would use; this repository has no such
  process.
* a topology table that pairs each accelerator with the drives under its PCIe
  switch or root port, so that an accelerator reads only the extents on those
  drives
* an xnvme fs backend in xal. The requests it would use, index, watch and
  release, are in the server and in `Client` of `lib/protocol.hpp`. The C
  interface does not offer them. xal's extents would also need to name the
  member they lie on, or its reads would have to go through the striping of
  xnvme fs.
* restricting a client to the extents it has leased
* a writable view
