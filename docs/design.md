<!--
SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>

SPDX-License-Identifier: CC-BY-4.0
-->

# xnvme fs design

## Goals

1. DuckDB's database, write-ahead log and temporary (spill) files on raw NVMe,
   through DuckDB's `FileSystem` interface, with no change to DuckDB.
2. Drives kept at their bandwidth: deep asynchronous queues per thread, read
   ahead for scans, write behind for spills, no lock on the submission path.
3. Several drives from the start: extents striped across them, so bandwidth
   grows with the drive count.
4. Destinations other than host memory: a block can be read into, or written
   from, an accelerator's memory peer to peer, so a later scan operator on the
   GPU needs no new storage layer.
5. Correct before fast: durability of the write-ahead log, crash consistency of
   the layout, and a test suite that runs in CI without NVMe hardware.

## Non-goals, for now

Parquet and CSV files on the drives (they stay on a regular file system), a
general-purpose file system, engines beyond DuckDB and RocksDB, a writable FUSE view,
several pools per DuckDB process, and drives other than NVMe.

## Architecture: AiSIO with HOMI

xnvme fs is a demonstration of the AiSIO architecture: several independent
processes share NVMe controllers, each driving its own I/O queues, with one
process owning each controller. It targets xNVMe's `homi_series` branch, whose
uPCIe backend has that control plane: `homi serve --homi-id N --be upcie BDF`
owns a controller (the device, its DMA memory, the admin queue) and serves
clients on `/tmp/xnvme-homi-N.sock`; a client opens the same device with
`--be upcie --homi-id N` and gets queues of its own, or fails with `ENOENT` if
nobody serves it.

Three kinds of process take part:

| Process | Unit | Role |
|---|---|---|
| HOMI | `homi` (xNVMe homi_series) | owns the controllers on vfio and the IOMMU; hands out I/O queues |
| `xnvme fs serve` | `xnvme-fs@<pool>` | HOMI client; the pool's metadata authority: superblock, extent maps, leases; serves the metadata socket and a read-only FUSE view |
| DuckDB or RocksDB with its adapter (the DuckDB extension `xnvme_fs`, the RocksDB plugin `xnvme_fs`) | the application | HOMI client for data: its own I/O queues per thread; the socket of `xnvme fs serve` for metadata |

Later, a scan process on an accelerator joins as a further HOMI client on the
same controllers, registering the card's memory as a dma-buf: the peer-to-peer
path under the same metadata authority.

**Why not the kernel's file systems.** AiSIO interoperates with them through
HOMI, qublk (a block device for the kernel), XFS and xal (files resolved to
LBAs, FIEMAP and the like). That keeps the kernel in the picture and needs all
four. xnvme fs goes the other way: it owns the layout outright, no kernel
manages its I/O, and what it holds is made visible by a read-only FUSE mount
served by `xnvme fs serve`, a management view rather than an overlay on an existing
file system. Neither qublk nor xal is needed.

**Clients are database instances: DuckDB and RocksDB.** Two engines with
opposite access patterns, analytics with large block reads and bursty spills,
and an LSM store with log appends, many immutable files and background
compaction, show that the design is not tuned to one. Each reaches xnvme fs
through its own pluggable file layer, unchanged: DuckDB through its file system
extension interface, RocksDB through a `FileSystem` plugin loaded by URI. Each
instance owns its files and space; `xnvme fs serve` partitions a pool between
instances through leases, with an optional quota per instance, and enforces
the locks each engine asks for across processes. Engines tied to `mmap` or the
kernel page cache are out of reach.

**Metadata socket.** A Unix socket of `xnvme fs serve`'s (`/run/xnvme-fs/<pool>.sock`).
Neither engine's file interface has extents, only files and byte offsets;
extents are xnvme fs's own. The socket is a small file service both adapters
share: create, open, close, rename, list, lock, sync, lease, commit, free. The
adapter, inside the engine's process, opens and closes files on the socket, leases extents in large pieces (default 64 MiB),
maps the engine's file offsets into its leases itself, commits extent maps at
checkpoints or syncs, and frees files; reads and writes stay off the socket.
RocksDB needs more of it than DuckDB: renames (its `CURRENT` file),
directory listings and syncs, a `LOCK` file, and thousands of files created
and deleted as compaction runs. `xnvme fs serve` writes
the superblock and the maps on its own queues, copy-on-write with a generation
number.

**FUSE view.** `xnvme fs serve --view /mnt/aisio/<pool>` mounts the pool
read-only and lists its files: databases, their WALs and spill files, each at
the size `xnvme fs serve` last heard from its client, committed, truncated or closed.
Reads go through `xnvme fs serve`'s own queues, at the same time as DuckDB's I/O on
the same controller, which is the point of showing it; files are opened for
direct I/O, so the kernel caches none of what the engines change underneath.
`xnvme fs serve` speaks the FUSE protocol on `/dev/fuse` itself, with ufuse
(github.com/safl/ufuse, `third_party/ufuse`), a header-only library with no
dependency beyond the kernel's `<linux/fuse.h>`; the descriptor is one more in
its poll loop, so a read through the view holds up metadata requests for its
duration, at most 128 KiB. Mounting needs `CAP_SYS_ADMIN`.

**Readers outside the engines.** A process that reads what the engines write,
xal indexing a pool for OpenDS, takes an index on the socket: every file's
size and extents, and where each member's drives lie on their namespaces,
under a read lease. It addresses the drives itself through HOMI. While the
lease is held `xnvme fs serve` gives no extent the index names to another file: an
extent freed by a truncate or a removal waits until every lease older than the
freeing is released or its connection closes. A watch on the lease, a request
on a connection of the reader's own, is answered once a change to names,
sizes or extents makes the index stale, which is the lease's recall; the
reader then marks its index dirty and takes a new one. Bytes an engine
overwrites in place change under the reader without a recall, as they would
under a reader of a kernel file system; the lease keeps extents, not their
contents.

**Failure.** DuckDB dying: `xnvme fs serve` reclaims the extension's leases and spill extents when
its connection closes. `xnvme fs serve` dying: the maps on the drive are the truth; a
restarted daemon reopens them and DuckDB's next open goes through it.

**Trust.** HOMI's queues reach the whole namespace; a client is trusted not to
write outside its leases. Fencing clients to their extents needs namespaces or
IOMMU work and is out of scope.

## A model for the kernel

xnvme fs doubles as a model of what a kernel file system would need to offer
for AiSIO to do I/O on its files directly: regions assigned to clients,
extents handed out under a lease that keeps them stable while held, allocation
ahead of writes, maps committed at sync, and leases reclaimed or recalled.
Today the same knowledge comes from FIEMAP, which is advisory, or from parsing
on-disk metadata (xal for XFS). XFS's layout leases for pNFS block layouts are
the nearest existing mechanism and worth studying (to verify).

## Names

xnvme fs names the engine-neutral parts: `xnvme fs serve`, the on-drive layout, the
socket protocol and the FUSE view. Each engine's binding is an adapter:
the DuckDB extension `xnvme_fs` and the RocksDB `FileSystem` plugin
`xnvme_fs`. AiSIO names the architecture they demonstrate:
once the mapping from a file's bytes to blocks on the drives is known, any
process, and later any accelerator, can do I/O on them directly and inspect
them through a plain view.

## Where it plugs into DuckDB

DuckDB routes database, write-ahead log, temporary files and external files
through its `VirtualFileSystem`, which takes extra file systems with
`RegisterSubSystem` (`common/virtual_file_system.cpp`). Requests are
synchronous `pread` and `pwrite` on the calling thread
(`common/local_file_system.cpp`); database blocks are 256 KiB, spill blocks 32
to 256 KiB, and there is no read-ahead for spills. xnvme fs registers for an
`xnvme-fs://` prefix: `ATTACH 'xnvme-fs://pool0/tpch.db'` and
`SET temp_directory = 'xnvme-fs://pool0/tmp'`. (File references are to DuckDB
v2.0.0; recheck at each DuckDB upgrade.)

## Layout

A *pool* is one or more NVMe namespaces, its *members*. Each member starts with
a superblock naming the pool, its members, its layout version (2), its extent
size (default 64 MiB) and its geometry. Two table slots follow the superblock,
then the data area, cut into extents. Only member 0's slots are used; every
member reserves them, so all members share one data offset.

The *table* is the pool's namespace and extent maps: every directory, and every
file with its durable size and its extents in file-offset order. It is written
whole, copy-on-write, to the slot its generation selects, and flushed. Its
header carries the pool's UUID and a CRC-32, so a torn write or a table left by
an earlier pool on the same drive is not read back; on start, `xnvme fs serve` takes
the newer valid slot.

xnvme fs keeps one kind of file. What differs between DuckDB's database, its
write-ahead log and its spill files is when DuckDB syncs them, and that alone
decides what reaches the table:

| File | Synced by DuckDB | In the table |
|---|---|---|
| database | at checkpoint | from its first sync, with the size of its last |
| write-ahead log | at every commit | the same |
| spill file | never | never; removed when its client's connection ends |

A file enters the table at its first commit or when renamed into place, and
leaves it when removed. One never committed outlives the client that wrote it,
readable by the next, as a closed file stays in the page cache; only losing
`xnvme fs serve` loses it, as a crash loses what was never synced. Its size there is the size at its last commit, not at
its last write. Extents a file no longer holds, after a truncate or a removal,
are free only once a table without them is on the drive. A file keeps the
block holding its end in memory, so an append starting or ending inside a
block, a log record, is written from it as whole blocks with no read: past the
end of a file the drive holds zeros, since leased extents are cleared and a
truncate clears the rest of the block it cuts. Such an append is written before
it returns, under the file's lock, so two appends to one block reach the drive
in order. The block and the appends after it are kept in DMA memory of one command's
size, the stage, so an append is copied once, into it, and written from it.
With `XNVME_FS_APPEND_BUFFER` set, a file leaves appends smaller than a command
there, serving reads of them from it, and writes them at a sync, a truncate,
its close, a write reaching the block that holds the file's end, or when the
stage is full, as one command; a larger append is written as it comes. Faster,
but what the engine flushed is lost if the process dies, and a handle's close
reports a failure to write them. `xnvme fs serve` clears an
extent with Write Zeroes before it leases it out, so a range of a file nothing
wrote reads as zeros and no file sees what an earlier one left.

## I/O

Each DuckDB worker thread gets its own xNVMe queue on each member drive,
created on first use, so submissions need no lock. DuckDB's calls stay
synchronous; xnvme fs hides latency behind them:

- **Reads.** A file handle detects sequential access and keeps a configurable
  number of extents' blocks in flight ahead of the reader, in a per-handle
  window. A read that hits the window waits only for its own blocks.
- **Spill writes** are acknowledged once copied into a write-behind buffer and
  written in large, stripe-aligned requests; reads of a spill file check the
  buffer first.
- **Database writes** at checkpoint are issued asynchronously and drained at
  `Sync()`; WAL appends are written through.

**Queues are a budget.** In the kernel, blk-mq maps per-CPU software queues onto
however many hardware queues the controller grants, so applications never see
that number. xnvme fs gives each thread a queue of its own on each drive, so the
controller's count bounds the threads of every client of the drive together,
`xnvme fs serve`'s included. Controllers differ widely in how many I/O queues
they grant, and HOMI also limits the queue pairs of one client over all its
controllers. A thread that finds none fails with an error saying so. Sharing a
queue between threads behind a lock, a software queue in user space, or HOMI
multiplexing clients onto fewer hardware queues, would lift the bound at a cost
per command.

The backend xnvme fs is designed for is xNVMe's uPCIe through HOMI: the drives
are bound to vfio-pci and held by HOMI, and DuckDB's threads and `xnvme fs serve` drive
queues of their own as its clients, with no kernel driver in the data path.
For the evaluation, the same library also reaches the drives through the
kernel's nvme driver (`Access.backend`, `XNVME_FS_BACKEND`), each step changing
one thing: `io_uring` reads and writes a namespace's block device through the
page cache, `io_uring_direct` without it, and `io_uring_cmd` passes the very
NVMe commands xnvme fs builds for HOMI through the namespace's character
device, so only who owns the queues differs from uPCIe. md sets stay usable:
mdadm makes and inspects them on the block devices while xnvme fs maps them
through the character devices, provided the kernel's array is not assembled
for writing meanwhile. xnvme fs waits for completions as it does on HOMI,
polling its queues paced, so the backend is the only thing that changes;
Write Zeroes, which io_uring's reads and writes cannot express, falls back to
writing zeros, and a sync becomes an fsync of the device.

Polling the kernel's backends gives up what they offer: the kernel takes the
drive's interrupt and can wake a waiting thread, through an eventfd on the
ring or io_uring's own wait for completions. A wait mode that sleeps until
then is still to come, chosen like `XNVME_FS_POLL_MODE` rather than by
backend.

## Partitions, mkfs and mount

A pool member is a controller held by HOMI, `0000:84:00.0`, or one partition of
its GPT, `0000:84:00.0/p2`. xnvme fs follows the conventions every Linux file
system does: the drive is partitioned with `sgdisk`, `fdisk` or `parted` while
the kernel holds it, the xnvme fs partition taking the type
`a04faa88-135a-4457-b8f6-db8bbaf344b8`, and `lsblk` shows it beside the others.
Once HOMI holds the controller, xnvme fs reads the GPT itself, checks both of its
checksums, refuses a partition of any other type, so a mistyped number never
formats someone's XFS, and offsets every command by the partition's start.

`mkfs.xnvme-fs` and `mount.xnvme-fs` are the program `xnvme-fs` under the names `mkfs(8)` and
`mount(8)` look for, taking their command lines:

```
mkfs.xnvme-fs -L pool 0000:84:00.0/p2
mount.xnvme-fs 0000:84:00.0/p2 /mnt/pool       # or: mount -t xnvme-fs ...
umount /mnt/pool
```

The mount reads the pool's name and its other members from the superblock of
the one it is given, starts `xnvme fs serve` for the pool with the read-only view at
the mount point, and returns once the view is mounted; the daemon logs to
`/run/xnvme-fs/POOL.log` and stops when the view is unmounted. The other
partitions of the drive are untouched but out of reach while HOMI holds the
controller, which therefore cannot be the drive the system runs from. The same
holds for any device HOMI serves, local or behind NVMe/TCP or NVMe/RDMA: xnvme fs
opens what it is told to open.

## Stripe sets

A member may also be an md RAID 0 set, named by its drives joined by `+`,
`0000:82:00.0+0000:83:00.0`, each a controller or a partition of the type
Linux RAID. The set is made and inspected with the kernel's tools while the
kernel holds the drives, and handed to HOMI once mdadm has stopped it:

```
mdadm --create /dev/md0 --level=0 --raid-devices=2 /dev/nvme0n1 /dev/nvme1n1
mdadm --stop /dev/md0
mkfs.xnvme-fs -L pool 0000:82:00.0+0000:83:00.0
```

xnvme fs reads md's superblock, format 1.2, from each drive, checks its
checksum and that the drives are all of one set, orders them by their roles,
and maps chunk c of the array to chunk c / N of drive c % N, as the kernel's
md driver does. Each thread has a queue of its own on each drive, so a request
spanning chunks has every drive it touches working at once. What xnvme fs cannot
map exactly as md does it refuses: another level, a reshape, drives that differ
in size once rounded to whole chunks, and any other md feature. With HOMI
stopped, the kernel assembles the set again and sees the pool's bytes where
xnvme fs wrote them.

Extents stay the unit of allocation, now over members that may each stripe.
A set spreads every file over all its drives; several sets in one pool keep
placement per set, which is what lets a unit of one accelerator and the drives
under its switch read only its own extents. The kernel offers the composite
mapping nowhere: on XFS over md, FIEMAP gives offsets in the array, and turning
them into drives and blocks also takes md's geometry, either of which may move
blocks.

## Small appends and the page cache

xnvme fs is direct by nature: every read and write goes to the drive, unless it is
asked to do otherwise. An engine on a kernel file system leans on the page cache
by default, and the page cache does a great job of hiding the drive's latency.
Which of the two caches is one of the questions database design keeps evolving
around; an engine that leans on the page cache has chosen not to do that caching
itself, and on xnvme fs the choice is explicit, the file layer doing it or not.

The engines' logs are appended to in small records: RocksDB's write-ahead log in
records smaller than a block. A kernel file system takes such an append into its
page cache and returns; the drive sees the data at the next sync or when the
kernel writes it back. xnvme fs has no page cache, so each append is a write to
the drive, whole blocks from the cached tail block but still a command and its
completion. With a database small enough to fit in the host's memory, a kernel
file system also serves its reads from the page cache, with no command to the
drive.

Small appends also amplify writes. Written as it comes, a record smaller than a
block is a read-modify-write: a block read and a block written. The cached tail
block removes the read but not the rewrite: each record rewrites the whole 4 KiB
block holding it, about 30 times before the block is full, and the drive's own
garbage collection multiplies that again. Buffering until a sync writes each
block once.

For the writes, xnvme fs offers the one part of a page cache a log needs: with
`XNVME_FS_APPEND_BUFFER` a file holds its appends in memory and writes them at
a sync, a truncate, its close, a write reaching the block holding the file's
end, or once they fill one command. Reads of them are served from memory,
writes below that block pass beside them, and an append of a command or more
is written as it comes. The buffer holds one command's worth, the size at
which a drain becomes a single write. It keeps the engine's durability at a
sync, as the page cache does. It gives up one thing the page cache keeps: a
process that dies loses what it buffered, where the kernel would still write
it back, which is why it is asked for, not the default. It caches nothing for
reads; an engine's own cache, RocksDB's block cache or DuckDB's buffer pool,
is what serves those.

## Memory and peer-to-peer

Every request names a *buffer*: an address range in an I/O address space and,
where the CPU can reach it, a host pointer. Host buffers come from DMA-able
memory registered once (hugepages). Accelerator buffers come from the
accelerator's driver as a dma-buf of device memory in the card's BAR. The card
stays in the iommufd context of the driver's device server; the client hands
the dma-buf to HOMI, which maps it into the one IOAS its controllers share
with `IOMMU_IOAS_MAP_FILE`, so a drive's DMA lands in device memory by its
IOVA. HOMI drops the mapping when the client unregisters it or exits, which
must happen before the driver frees the buffer, since the driver cannot revoke
an exported dma-buf. DuckDB's own calls always use host buffers. A separate
interface, used by a scan operator on an accelerator, asks for extents of a
file to be read into a device buffer; the extent map tells it which drive
holds each one, and a topology table pairs each accelerator with the drives
under its PCIe switch or root port, so a unit of one GPU and the drives that
fill its link reads only its own stripes.

## Testing and CI

- **No hardware in CI.** A QEMU guest's emulated NVMe controllers, bound to
  vfio-pci and held by HOMI, carry the whole extension, layout and I/O path on
  GitHub's runners.
- DuckDB's sqllogictest suite run with the database and `temp_directory` on
  xnvme fs; TPC-H at scale factor 1 checked against DuckDB's reference answers,
  with a small `memory_limit` so it spills.
- Crash consistency: a test that kills the process at random points during
  checkpoints and WAL appends and checks that the database reopens to a
  committed state.

## Build

A DuckDB extension built with DuckDB's extension template and CI tools,
against a pinned DuckDB release; xNVMe as a dependency with its uPCIe backend.
Building DuckDB is heavy, so builds run in CI and on lab hosts, not on a
workstation.

The pins are git submodules: `duckdb` at v2.0.0, `extension-ci-tools` on
its `v2.0-cyanoptera` branch, and `third_party/rocksdb`. The adapters take
libxnvme-fs by pkg-config, as the meson project at the top of this repository
installs it into the prefix of an xNVMe with uPCIe and HOMI. Run on its own
rather than from DuckDB's build, `cmake -S adapters` builds only what needs no
DuckDB, `xnvme-fs-bench`.

`xnvme fs` is the program `xnvme-fs`, which that meson project builds. `xnvme fs mkfs
--pool NAME --member BDF... --homi-id N` formats a pool; `xnvme fs serve`
serves it on `/run/xnvme-fs/NAME.sock` or `--socket`; `xnvme fs info` prints
its superblock and table. The daemon locks each member through a file named
for it under `/run/lock`, so two daemons never format or serve one
controller. It handles one request at a
time, table writes included, and exits when a table write fails, since memory
and drive then disagree and the drive is the truth.

The extension finds the socket through `xnvme_fs_socket_dir`, which defaults to
`XNVME_FS_SOCKET_DIR` and then `/run/xnvme-fs`, and opens the members `xnvme fs serve`
names through uPCIe, the default backend: a member is a controller named by its
PCI address, held by `homi serve`, and every process using the pool,
`xnvme fs serve`, `xnvme fs mkfs` and each DuckDB, opens it as a client of that
server, with `--homi-id` or `xnvme_fs_homi_id` (default `XNVME_FS_HOMI_ID`). No
kernel driver is in the data path. A member is driven with NVM read, write
and flush commands in whole blocks; a write covering a block in part reads,
merges and writes it back. Each thread has a queue of its own on each member,
made on first use and given back when the thread exits: sixteen commands of
at most 256 KiB, each with its buffer from the server's DMA heap, so a request
larger than one command has all of its commands in flight at once.

The engine-neutral library under `lib/` carries everything an adapter needs: a
session with a pool's `xnvme fs serve`, the members it opened, and files shared
by every handle a process has on a name. Its interface is asynchronous at its
core. `File::Submit` starts a request, a read or write of a range of a file: it
splits the range at extent boundaries and starts every piece on its member's
queue, so a range spanning members has all of them working at once, and returns.
The thread that submitted it reaps it with `Poll`, which never waits, or `Wait`;
there is no background thread and no lock on the path. `Wait` polls without
interrupts, but paced: a request not done at once first sleeps for half of what
the thread's waits on requests of its size took, much as the kernel's hybrid
polling sleeps before it polls, so a reading thread does not hold its core for
the whole wait on the drive. Syncs and the writes a stage lands wait the same
way, each kind learning from its own. `XNVME_FS_POLL_MODE=spin` spins through
every wait instead, which takes more CPU time per request. `File::Read` and
`File::Write` are Submit and Wait. `libxnvme_fs.h` offers the same to C,
POSIX-like calls returning a negative errno. `xnvme-fs-bench` drives the
interface as the engines do, synchronously or with a chosen number of requests
in flight, with `--verify` stamping every sector with its offset and checking
it; CI runs it briefly as a correctness check.

DuckDB's storage opens files without the settings in reach, so the extension
keeps one connection per pool per process and reuses it.

The RocksDB plugin, in `rocksdb/`, is RocksDB's `FileSystem` on the same library,
reached by URI as `xnvme-fs://<pool>` (`db_bench --fs_uri=xnvme-fs://pool0`), with
RocksDB's paths naming files in the pool. It is built into RocksDB, pinned at
v11.8.0 (`third_party/rocksdb`), as a plugin: `make rocksdb` links the
directory in as `plugin/xnvme_fs`, whose `CMakeLists.txt` names the source, the
include path and the libraries pkg-config names for libxnvme-fs, and whose
`xnvme_fs.mk` the registration function. `MultiRead` has every read of a batch in
flight at once, and `ReadAsync` with `Poll` hands RocksDB the library's
requests. A directory's `Fsync` has nothing to do, since a file's commit, a
rename and a removal each reach the drive before they return, and `LockFile`
holds the lock file open, which `xnvme fs serve` lets one client do at a time, and
refuses a second lock within the process as RocksDB's POSIX file system does.
The pool cannot be mapped, so a file opened with `use_mmap_reads`, which RocksDB
does only for files it has finished writing and which its PlainTable format
needs, is read into memory at open and served from there.
RocksDB's file system conformance tests, its basic database tests and
`db_bench` run in CI's guest with `TEST_FS_URI=xnvme-fs://ci`. `xnvme_fs_pool_info(name)` reports a pool's members, extents and generation.

With `XNVME_FS_TRACE` naming a file, the library appends a line for every
operation an engine asks of it, through either adapter: open, read, write, an
asynchronous read or write submitted, sync, truncate, size, close, stat, list,
mkdir, rmdir, remove and rename, and beneath them the extent protocol's leases
and commits. A line holds the time, process and thread, the operation, the name,
an offset or size, and the bytes. The cijoe step `test_trace` traces DuckDB
through TPC-H SF1 and RocksDB through `db_bench`, and prints for each engine the
operations it used, how often, and the sizes of its reads and writes.

The cijoe step `xnvme_fs_build` copies the checkouts of xNVMe and of this
repository to the machine it targets and builds xNVMe, xnvme fs, the
extension, `xnvme-fs-bench` and a Release `db_bench` in a tree on it.

The guest suites also run without GitHub:
`cijoe/workflows/xnvme-fs-build.yaml` with `configs/local.toml` builds the
tree; `xnvme-fs-guest.yaml` then runs every suite in the QEMU guest as CI
does, off the machine's own drives. The guest needs KVM; under WSL2 that takes
nested virtualization.

Every test runs on controllers held by HOMI. The cijoe workflow
`xnvme-fs-guest`, which CI runs after the build, boots a QEMU guest with an
IOMMU and two emulated NVMe controllers bound to vfio-pci, starts `homi
serve` on them, and runs each suite as a step on a pool of both: a member's
I/O, the sqllogictests beside `xnvme fs serve`, TPC-H SF1 among them, the daemon's
command line, the FUSE view, the crash test and DuckDB's storage tests. The
same targets run on any host with `XNVME_FS_MEMBER` and `XNVME_FS_HOMI_ID`
set; they overwrite the controllers. Two more steps run the C interface's test
and the benchmark's correctness matrix.

## Milestones

1. **Skeleton.** Repository, REUSE, pre-commit, CI building the extension
   against a pinned DuckDB and running a trivial test on the file backend.
   Done.
2. **`xnvme fs serve` and one drive.** The daemon with the pool, superblock, extent
   maps and the metadata socket; DuckDB's database, WAL and temporary files
   through it, synchronous I/O; sqllogictests and TPC-H SF1 green; crash test.
   Done; its tests run on controllers held by HOMI, in CI's QEMU guest.
3. **HOMI and the view.** xNVMe homi_series: HOMI owns the controller,
   DuckDB's threads and `xnvme fs serve` are clients with queues of their own; the
   read-only FUSE view; read-ahead and write-behind on deep queues.
4. **RocksDB.** The `FileSystem` plugin over the same socket; renames,
   listings, locks; RocksDB's own tests and `db_bench` on xnvme fs.
5. **Several drives.** Members that are md RAID 0 sets, made and inspected
   with mdadm and mapped in user space; scaling from one to four drives
   against XFS on the same set; a DuckDB and a RocksDB instance on one pool at
   once, with the FUSE view showing both.
6. **Peer-to-peer.** A scan process on an accelerator as a HOMI client, device
   buffers registered as dma-bufs, topology table, the read-into-device
   interface. The interface is done: `xnvme_fs_register_dmabuf` and
   `xnvme_fs_submit_direct`. Registration waits on xNVMe's dma-buf series; the
   topology table is open.
7. **xal and OpenDS.** An xnvme fs backend in xal: a client of `xnvme fs serve`'s
   socket that indexes a pool's files and their extents under a recallable
   read lease, whose recall marks the index dirty, so OpenDS reads xnvme fs
   files into GPU memory through xal and HOMI with no kernel file system.
   `xnvme fs serve`'s index, watch and read lease are done; xal's extents gain the
   member they lie on, or reads go through xnvme fs's striping.
