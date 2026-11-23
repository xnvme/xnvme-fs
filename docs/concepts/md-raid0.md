<!--
SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>

SPDX-License-Identifier: CC-BY-4.0
-->

# md RAID 0 members

A member can be an md RAID 0 set: a group of drives that the md driver of the
Linux kernel stripes as one array. Create and inspect the set with `mdadm`
while the drives are bound to the kernel's nvme driver. xnvme fs reads the md
metadata from the drives and maps the offsets of the array to the drives as
the md driver does. Under uPCIe, no kernel driver is in the data path.

## Naming a set

The name of a set is the names of its drives joined by `+`, in any order:
`0000:82:00.0+0000:83:00.0` under uPCIe, `/dev/ng0n1+/dev/ng1n1` under
`io_uring_cmd`. Each drive is a whole namespace or a GPT partition named with
`/pN`. The partition must have the type Linux RAID,
`a19d880f-05fc-4d3b-a006-743f0f84911e`, or the type of xnvme fs. Under uPCIe,
the name of a drive cannot start with `/`.

## Making a set

Create the set with `mdadm`, and stop the array before the drives are bound to
vfio-pci for the HOMI server:

```
mdadm --create /dev/md0 --level=0 --raid-devices=2 /dev/nvme0n1 /dev/nvme1n1
mdadm --stop /dev/md0
mkfs.xnvme-fs -L pool 0000:82:00.0+0000:83:00.0
```

Under the kernel's backends, `mdadm` can create and inspect the set on the
block devices while xnvme fs maps the set through the same namespaces. The
kernel must not have the array assembled for writing at the same time.

## What xnvme fs checks

xnvme fs reads the md superblock of format 1.2 at offset 4 KiB of each drive
and verifies the following on each drive:

* the magic, the major version 1, and that the superblock records its own
  offset as 8 sectors
* the checksum in `sb_csum`
* that the level is 0, and that the feature map has no bit set other than
  `MD_FEATURE_RAID0_LAYOUT`
* that the role of the drive is that of an active drive of the set
* that the chunk is a whole number of the drive's blocks
* that the data area lies within the drive and starts on a block boundary

Across the drives, xnvme fs verifies that every drive has the same set UUID,
number of drives, chunk size and event count, that the number of drives named
equals the number of drives of the set, that no two drives have the same role,
that all drives have the same block size, and that the data areas have the
same size after rounding down to whole chunks. The open fails with `ENOTSUP`
for a level other than 0, for any other feature, such as a reshape in
progress, and for drives whose data areas differ in size. Every other failed
check fails the open with `EINVAL`.

## The mapping

xnvme fs orders the drives by their roles in the set. xnvme fs addresses each
drive from the start of the drive's md data area, over the drive's data size
rounded down to whole chunks. With N drives and a chunk of C bytes, chunk `c`
of the member is chunk `c / N` of drive `c % N`. The md driver maps RAID 0
over drives of one size in the same way. The size of the member is N times the
rounded data size of one drive.

xnvme fs splits a read or write on the member at chunk boundaries and submits
each piece to its drive, on the calling thread's queue for that drive. A
request that spans chunks therefore has commands in flight on every drive it
covers at the same time. A flush sends the NVMe Flush command to every drive.
The largest command on the member is the smallest of the largest commands of
its drives. A range of the member that is cleared to zeros is one contiguous
range on each drive. xnvme fs allocates DMA memory and registers a dma-buf
through the first drive of the set.

After the HOMI server has stopped and the drives are bound to the kernel's
nvme driver again, `mdadm --assemble` assembles the array, and the array
contains the pool's bytes at the offsets where xnvme fs wrote them.

## Readers of the index

The [index](leases.md#read-leases) contains each member's md chunk size in
bytes, which is 0 for a member that is not a set. The index lists the drives
of the member in the order of the mapping. For each drive, the index gives the
byte offset at which the member's data starts on the drive: the start of the
partition plus the md data offset. A reader that submits its own I/O to the
drives maps the offsets of the member to the drives with these values.
