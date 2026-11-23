<!--
SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>

SPDX-License-Identifier: CC-BY-4.0
-->

# Extents and striping

The data area of every member is divided into extents of `extent_size` bytes.
`mkfs` sets `extent_size` for the pool: a power of two of at least 64 KiB, and
64 MiB unless `--extent-size` or `-E` gives another value. The server
allocates space to files in whole extents, and the bytes of a file are placed
on the members extent by extent.

## Extent addresses

An extent has a 64-bit address, `ExtentAddr` in `lib/layout.hpp`. The top 8
bits hold the member's index, and the low 56 bits hold the extent's index on
that member. Extent `index` of a member starts at byte
`data_offset + index * extent_size` of the member.

## A file's map

A file's map is the list of the file's extents, in the order of the file's
offsets. Byte `o` of the file is in entry `o / extent_size` of the map, at
offset `o % extent_size` in that extent. The server stores the map of every
file. The server sends a file's map to the client that opens the file, and
appends to the map when the client leases more extents. The client computes
the location of every byte from its copy of the map, without a request to the
server.

A range of the file past the last extent of the map is a hole. A read of a
hole returns zeros and sends no command to a drive. A truncate that makes a
file larger leases no extents. The part of the added range that lies past the
file's last extent is a hole until a write leases extents for it.

## Allocation over the members

The server allocates the extents of a lease one at a time. Each extent is the
lowest free extent of the member with the most free extents. Among members
with equally many free extents, the server selects the member with the lowest
index. With members of equal size and equal free space, consecutive extents of
a file are therefore on consecutive members, and a file of N or more extents
has extents on all N members. A lease of more extents than the pool has free
fails with `ENOSPC` and allocates none.

Before the server answers a lease, it clears every allocated extent with the
NVMe Write Zeroes command. Where that command fails, the server writes zeros
instead. A range of a file that was never written therefore reads as zeros,
and a file never reads the data that an earlier file wrote to the extent. When
clearing fails, the server frees the allocated extents and the lease fails.

## Requests over several members

The client splits every read and write at extent boundaries. The client
submits each piece to the member that holds the piece's extent, on the calling
thread's queue for that member. A request that spans extents on different
members has commands in flight on all of those members at the same time, and
completes when the last command completes. A member that is an md RAID 0 set
splits each piece again at the chunk boundaries of the set and submits the
parts to the drives of the set; [md RAID 0 members](md-raid0.md) describes
that mapping.

## Freeing extents

The server removes extents from a file's map when a truncate makes the file
shorter, when the file's last handle closes while the file has extents past
its size, and when the file is removed and no handle is open on it. When the
file is in the table, the table on the drive may still contain a removed
extent. The server keeps such an extent allocated until it has written a table
that does not contain the extent. [Leases and commits](leases.md) describes
when the server writes the table.
