<!--
SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>

SPDX-License-Identifier: CC-BY-4.0
-->

# Pools and members

A pool consists of 1 to 16 members that `xnvme fs mkfs` or `mkfs.xnvme-fs`
formats together. Every member starts with a superblock that holds the pool's
name and the names of all members, in order. A command that opens a pool names
all members in that order. `mount.xnvme-fs` names one member and reads the
names of the other members from that member's superblock.

A pool's name is 1 to 63 characters, each a letter, a digit, `_`, `-` or `.`.
A member's name is 1 to 127 characters. A pool cannot name the same member
twice.

## What a member is

A member is a namespace, a partition of a namespace, or an md RAID 0 set. The
name of a member depends on the backend that opens it; [Access](access.md)
describes the backends.

| Member | `upcie` | `io_uring`, `io_uring_direct` | `io_uring_cmd` |
|--|--|--|--|
| A namespace | `0000:03:00.0`, the PCI address of a controller that a HOMI server serves | `/dev/nvme0n1` | `/dev/ng0n1` |
| Partition N of its GPT | `0000:03:00.0/p2` | `/dev/nvme0n1/p2` | `/dev/ng0n1/p2` |
| An md RAID 0 set | `0000:03:00.0+0000:04:00.0` | `/dev/nvme0n1+/dev/nvme1n1` | `/dev/ng0n1+/dev/ng1n1` |

A name that ends in `/pN`, where N is 1 or greater, names partition N of the
namespace named before `/pN`. The partition must have the GPT partition type
of xnvme fs, `a04faa88-135a-4457-b8f6-db8bbaf344b8`. To open the partition,
xnvme fs reads the GPT header from block 1 of the namespace, verifies the
CRC-32 of the header and the CRC-32 of the partition entries, and verifies the
type of entry N. Offset 0 of the member is then the first block of the
partition, and the member ends at the last block of the partition. The open
fails with `EINVAL` for a partition of another type, a missing GPT or a failed
checksum, and with `ENOENT` for a partition number greater than the number of
entries in the GPT. Create the partition with a tool such as `sgdisk` while
the drive is bound to the kernel's nvme driver.

A name that contains `+` names the drives of an md RAID 0 set; [md RAID 0
members](md-raid0.md) describes those members.

## The layout of a member

Every member has the same layout, with the same offsets:

| Offset | Size | Content |
|--|--|--|
| 0 | 4 KiB | The superblock |
| 4 KiB | `slot_size` | Table slot 0 |
| 4 KiB + `slot_size` | `slot_size` | Table slot 1 |
| `data_offset` | to the end | The data area, divided into extents of `extent_size` |

The table is stored only in the table slots of member 0. The other members
reserve the same space and do not use it, so the data area starts at the same
offset on every member. [Leases and commits](leases.md) describes the table.

`mkfs` computes the geometry. `slot_size` is 1 MiB plus 8 bytes for each
extent of the pool, rounded up to a multiple of 4 KiB. For this computation,
the number of extents of the pool is the size of the smallest member divided
by `extent_size`, rounded down, times the number of members. `data_offset` is
4 KiB plus two slots, rounded up to a multiple of 1 MiB. A member holds
`(size - data_offset) / extent_size` extents, rounded down, where `size` is
the member's size in bytes. `mkfs` fails with `EINVAL` when the smallest
member is smaller than `data_offset` plus one extent.

## The superblock

The superblock is the first 4 KiB of a member. Integers are little-endian, and
names are padded with NUL bytes:

| Offset | Size | Field |
|--|--|--|
| 0 | 8 | The magic, `XNVMEFS` and a NUL byte |
| 8 | 4 | The layout version, 2 |
| 12 | 4 | The index of this member in the pool |
| 16 | 4 | The number of members |
| 24 | 16 | The pool's UUID, which `mkfs` generates at random |
| 40 | 8 | `extent_size` in bytes |
| 48 | 8 | `slot_size` in bytes |
| 56 | 8 | `data_offset` in bytes |
| 64 | 64 | The pool's name |
| 128 | 16 x 128 | The members' names, in the pool's order |
| 4092 | 4 | The CRC-32 of bytes 0 to 4091 |

The CRC-32 is the reflected CRC-32 with the polynomial `0xEDB88320`, which
zlib also computes. The checksum of the table and the GPT checks use the same
function.

## Formatting a pool

`mkfs` verifies the names and verifies that the extent size is a power of two
of at least 64 KiB. The default extent size is 64 MiB. `mkfs` opens every
member and fails with `EEXIST` when a member starts with the superblock's
magic, unless `--overwrite`, or `-f` for `mkfs.xnvme-fs`, is given. `mkfs`
then does the following, in this order:

1. It generates a random UUID for the pool.
2. It writes zeros over the first 4 KiB of both table slots of member 0, so
   that the server cannot read a table that an earlier pool wrote there.
3. It writes an empty table with generation 1.
4. It writes the superblock of every member and flushes the member.

## Opening a pool

`xnvme fs serve` and `xnvme fs info` read the superblock of every member and
verify the following:

* the magic, the layout version and the checksum
* that the pool's name is the name given
* that the list of members is exactly the list given, in the same order
* that the member's index is the member's position in that list
* that the UUID, `extent_size`, `slot_size` and `data_offset` equal those of
  member 0
* that the member is not smaller than `data_offset`

Each failed check fails the open with `EINVAL` and a message that names the
member.

`xnvme fs serve` then reads the table and keeps the pool's metadata in memory
until the process exits. [The server and its protocol](server.md) describes
that process.
