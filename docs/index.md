<!--
SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>

SPDX-License-Identifier: CC-BY-4.0
-->

# xnvme fs

xnvme fs is a file system for databases on NVMe drives driven from user space.
DuckDB and RocksDB keep their files on it unchanged, through their own
pluggable file layers. Each engine thread drives NVMe queues of its own on
controllers that HOMI shares between processes, and a small server, `xnvme fs serve`,
hands out extents under leases and commits each file's map at sync. A read-only
FUSE view shows a pool's files as ordinary ones.

It is the file system of the AiSIO architecture: xNVMe for the I/O interface,
uPCIe for user-space NVMe and the IOMMU setup shared with accelerators, and
HOMI for sharing a controller between independent processes. Whoever knows the
mapping from a file's bytes to blocks can do I/O on them from any process and,
later, from any accelerator; xnvme fs is the simplest file system that provides
that mapping.

A pool is a set of members, whole controllers, GPT partitions of them or md
RAID 0 sets of either, striped by extent. A partition is made with the usual tools while the kernel
holds the drive and typed as xnvme fs's own, `mkfs.xnvme-fs` formats it, and
`mount.xnvme-fs` serves it.

```{toctree}
:maxdepth: 2
:caption: Using xnvme fs

getting-started
tools
engines
configuration
```

```{toctree}
:maxdepth: 2
:caption: Inside xnvme fs

design
reproducing
```
