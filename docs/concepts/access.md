<!--
SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>

SPDX-License-Identifier: CC-BY-4.0
-->

# Access through uPCIe and HOMI, or io_uring

Every process that uses a pool, the server and each client, opens the pool's
members and submits NVMe commands to them through xNVMe. The backend
determines how a process opens a member. The server and every client of a pool
must use the same backend, because a client opens the member names that the
server returns in `HELLO`.

## The backends

| Backend | Member | How xNVMe opens it |
|--|--|--|
| `upcie` | a controller that a HOMI server serves, named by PCI address | the backend `upcie`, as a client of the HOMI server with the given HOMI id |
| `io_uring` | a namespace's block device, `/dev/nvme0n1` | the backend `linux` with io_uring as the asynchronous interface, through the page cache |
| `io_uring_direct` | a namespace's block device | the same, with the device opened for direct I/O, without the page cache |
| `io_uring_cmd` | a namespace's character device, `/dev/ng0n1` | the backend `linux` with io_uring passing the NVMe commands through the character device |

`--backend`, or the `backend` argument of `xnvme_fs_connect()`, selects the
backend. Without either, the environment variable `XNVME_FS_BACKEND` selects
the backend, and without that variable the backend is `upcie`. Any other
backend name fails with `ENOTSUP`, and `upcie` without a HOMI id fails with
`EINVAL`.

Under `upcie`, the drives are bound to vfio-pci and a HOMI server,
`xnvme homi serve` of xNVMe, serves the controllers. Every process that uses
the pool, including the server of the pool, is a client of that HOMI server
and receives its own queues from it. No process opens a controller directly,
and no kernel driver is in the data path. The DMA memory of a process's queues
is allocated from the heap of the HOMI server, which has one heap for all the
controllers it serves.

Under the kernel's backends, the drives are bound to the kernel's nvme driver
and there is no HOMI server. `io_uring_cmd` passes the NVMe commands that
xnvme fs builds, the same commands as under `upcie`, through the namespace's
character device. Under `io_uring` and `io_uring_direct`, xNVMe converts the
commands to reads and writes of the block device; a read or write that
transfers fewer bytes than requested fails with `EIO`. Under every backend,
xnvme fs writes zeros where the NVMe Write Zeroes command fails.

## Queues

Each thread of a process has its own queue on each drive that the thread uses.
xnvme fs creates the queue when the thread first submits a command to the
drive. Threads therefore do not wait for each other to submit. A queue has a
depth of 32 commands and one slot of DMA memory per command, and a slot has
the size of the largest command. xnvme fs allocates two slots with the queue,
and each other slot when a command first needs it. When the allocation of a
further slot fails, the queue continues with the slots it has. When the thread
exits, xnvme fs waits for the queue's commands in flight to complete and then
destroys the queue.

The largest command on a drive transfers the smallest of 65536 blocks, 256 KiB
and the controller's maximum data transfer size, rounded down to whole blocks.
xnvme fs splits a larger request into several commands, which are all in
flight at the same time.

A controller supports a limited number of I/O queues, and under `upcie` the
HOMI server also limits the number of queues of one client over all its
controllers. Every thread of every process that uses a drive has one queue on
the drive, including the thread of the pool's server. When the creation of a
queue fails, the error message states how many queues the process already has
on that drive. Run fewer threads on the pool to stay within the limit.

## Reads and writes

A read reads whole blocks into the slots of the queue. As each command
completes, xnvme fs copies the requested bytes from the slot into the caller's
buffer. The caller's buffer must stay valid until the read is done.

A write copies the caller's bytes into the slots before it returns, so the
caller can reuse its buffer immediately. A write that covers only part of a
block first reads the block into the slot, merges the new bytes into the slot
and writes the whole block. Such a write holds a lock of the drive from the
read until the write has completed, so two such writes never merge into the
same block concurrently. Such a write also waits until its queue has no
command in flight before it returns.

A command that completes with an NVMe error status fails its request with
`EIO`, and the message from `xnvme_fs_errmsg()` contains the status code type
and the status code.

## Appends and the stage

For a file that is appended to, xnvme fs keeps the block that contains the end
of the file in a buffer of DMA memory, the stage. The stage has the size of
one command. The first append reads that block into the stage when the block
contains bytes of the file. After that, xnvme fs copies an append that does
not start and end on block boundaries into the stage and writes the stage as
whole blocks, without a read before each write. The write of the stage
completes before the append returns and runs under the file's lock, so two
appends to one block reach the drive in order. The drive contains zeros past
the end of a file, because the server clears leased extents and a truncate
clears the rest of the extent it cuts. xnvme fs therefore fills the stage with
zeros past the end of the file and does not read those bytes from the drive.

When `XNVME_FS_APPEND_BUFFER` is set to a value other than the empty string or
`0`, an append smaller than one command remains in the stage and is not
written immediately. xnvme fs writes the stage at a sync, at a truncate, at
the close of the file, at a write other than an append that reaches the block
containing the end of the file, and when the stage is full. A read of the
buffered bytes returns them from the stage. The buffered appends are lost when
the process dies. `xnvme_fs_close()` returns the error of writing them when
that write fails. An append of the size of one command or larger is written
when it is submitted.

## Direct reads and DMA memory

`xnvme_fs_submit_direct()` reads whole blocks of a file directly into DMA
memory. The DMA memory is either memory from `xnvme_fs_dma_alloc()` or the
memory of another device, registered as a dma-buf with
`xnvme_fs_register_dmabuf()`. The CPU does not access the bytes, so a direct
read also works with memory that the CPU cannot access. The read fails with
`EINVAL` when its offset or length is not a multiple of the pool's block size,
when it extends past the last block of the file that is on the drives, or when
it covers a hole. `xnvme_fs_dma_alloc()` allocates memory through the first
member. Under `upcie`, the memory comes from the heap of the HOMI server, and
every member that the HOMI server serves can transfer to and from that memory.

A dma-buf is registered at a range of the process's virtual addresses that has
the size of the dma-buf. The caller reserves the range, for example with an
`mmap()` of `PROT_NONE`, and never accesses it. The registration requires that
xnvme fs was built against an xNVMe that provides
`xnvme_buf_register_dmabuf()`. The meson option `dmabuf` controls whether the
build requires that function. In a build without the function, the
registration calls return `-ENOTSUP`.

## Waiting for commands

xnvme fs polls its queues for completions and does not wait for interrupts,
under every backend. `XNVME_FS_POLL_MODE` selects how a thread waits. When the
variable is unset or `paced`, a thread whose request is not done at the first
poll sleeps for half of the average duration of its recent waits for requests
of the same size class, and then polls until the request is done. The size
classes are powers of two from 4 KiB, plus one class for flushes. A thread
tracks the wait duration of each class as a moving average in which the latest
wait has the weight 1/8, and sleeps only when the average is 10 microseconds
or more. When the request is done at the time the thread wakes, the thread
halves the average, so the sleep becomes shorter when a drive has become
faster. When the variable is `spin`, a thread polls for the whole wait. Any
other value fails the call that waits with `EINVAL`.
