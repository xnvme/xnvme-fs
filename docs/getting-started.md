<!--
SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>

SPDX-License-Identifier: CC-BY-4.0
-->

# Getting started

This takes one NVMe drive from a fresh partition table to DuckDB and RocksDB
keeping their files on it. It needs a Linux host with an IOMMU, iommufd and
vfio's device files, about 256 reserved 2 MiB hugepages, and the tree built as
[Reproducing](reproducing.md) describes. Every command runs as root.

## Partition the drive

While the kernel's `nvme` driver holds the drive, partition it with the usual
tools. The xnvme fs partition takes xnvme fs's type,
`a04faa88-135a-4457-b8f6-db8bbaf344b8`; the others stay what they are.

    sgdisk -n 1:0:+100G -t 1:8300 -c 1:xfs \
           -n 2:0:0 -t 2:a04faa88-135a-4457-b8f6-db8bbaf344b8 -c 2:xnvme-fs /dev/nvme0n1
    lsblk -o NAME,SIZE,PARTTYPE,PARTLABEL /dev/nvme0n1

## Hand the controller to HOMI

HOMI takes the whole controller, so the kernel loses every partition on the
drive while it runs: unmount them first, and never do this to the drive the
system runs from. Bind the controller to vfio-pci and start HOMI on it.

    bdf=0000:84:00.0
    echo vfio-pci > /sys/bus/pci/devices/$bdf/driver_override
    echo $bdf > /sys/bus/pci/devices/$bdf/driver/unbind
    echo $bdf > /sys/bus/pci/drivers_probe
    homi serve $bdf --be upcie --homi-id 1 --host_heap_size 1073741824 &

HOMI has one heap for every controller it serves. Each thread of each client
has a queue on every controller of its pool, which takes 4.5 MiB of the heap
(xNVMe's 4 MiB of PRP lists and two 256 KiB slots) and grows to 8 MiB as the
thread keeps more commands in flight; DuckDB starts one thread per CPU. A queue that finds the heap spent goes on with the memory
it has, so a heap too small for every queue at its deepest still works.

## Format and mount the pool

    export XNVME_FS_HOMI_ID=1
    mkfs.xnvme-fs -L pool $bdf/p2
    mkdir -p /mnt/pool
    mount.xnvme-fs $bdf/p2 /mnt/pool

`mount.xnvme-fs` reads the pool's name and members from the partition's
superblock, starts `xnvme fs serve` for the pool with its view at `/mnt/pool`, and
returns once the view is up. A pool of several members is formatted by naming
them all, `mkfs.xnvme-fs -L pool $bdf/p2 0000:85:00.0`, and mounted by naming
any one. A member striped over several drives is an md RAID 0 set made with
`mdadm --create --level=0` while the kernel holds them, stopped with
`mdadm --stop`, and named by its drives joined by `+`,
`mkfs.xnvme-fs -L pool 0000:82:00.0+0000:83:00.0`.

## Use it from the engines

DuckDB reaches the pool through paths `xnvme-fs://POOL/NAME`:

    duckdb -c "LOAD xnvme_fs; LOAD tpch; ATTACH 'xnvme-fs://pool/tpch.db' AS t;
               SET temp_directory = 'xnvme-fs://pool/tmp'; USE t; CALL dbgen(sf=1);"

RocksDB through its file system URI:

    db_bench --fs_uri=xnvme-fs://pool --db=/bench --benchmarks=fillrandom,readrandom

Both appear in the view, read-only, while they run:

    ls -l /mnt/pool

## Stop

    umount /mnt/pool
    kill %1                                   # HOMI
    echo nvme > /sys/bus/pci/devices/$bdf/driver_override
    echo $bdf > /sys/bus/pci/drivers/vfio-pci/unbind
    echo $bdf > /sys/bus/pci/drivers_probe

Unmounting the view stops `xnvme fs serve`. With the drive back under the kernel, its
other partitions return as they were.
