<!--
SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>

SPDX-License-Identifier: CC-BY-4.0
-->

# Getting started

This page goes from one NVMe drive with a new partition table to DuckDB and
RocksDB storing their files on the drive. The steps need:

* a Linux host with an IOMMU, iommufd and the device files of vfio
* 512 reserved 2 MiB hugepages, for the 1 GiB heap that HOMI is started with
  below
* the tree built as [Reproducing](reproducing.md) describes

Every command runs as root.

## Partition the drive

Partition the drive with the usual tools while it is bound to the kernel's
`nvme` driver. Give the partition for xnvme fs the GPT partition type of
xnvme fs, `a04faa88-135a-4457-b8f6-db8bbaf344b8`. The other partitions keep
their types.

    sgdisk -n 1:0:+100G -t 1:8300 -c 1:xfs \
           -n 2:0:0 -t 2:a04faa88-135a-4457-b8f6-db8bbaf344b8 -c 2:xnvme-fs /dev/nvme0n1
    lsblk -o NAME,SIZE,PARTTYPE,PARTLABEL /dev/nvme0n1

## Bind the controller to vfio-pci and start HOMI

HOMI needs the whole controller bound to vfio-pci. While the controller is
bound to vfio-pci, the kernel has no block device for the drive, so no
partition of the drive can be mounted by the kernel. Unmount the partitions
first, and do not use the drive that holds the running system. Bind the
controller to vfio-pci and start HOMI on it:

    bdf=0000:84:00.0
    echo vfio-pci > /sys/bus/pci/devices/$bdf/driver_override
    echo $bdf > /sys/bus/pci/devices/$bdf/driver/unbind
    echo $bdf > /sys/bus/pci/drivers_probe
    xnvme homi serve $bdf --be upcie --homi-id 1 --host-heap-size 1073741824 &

HOMI has one heap of DMA memory for all the controllers that it serves. Each
thread of each process that uses the pool, the threads of `xnvme fs serve`
included, has a queue on every controller of the pool.

For each queue, xnvme fs first allocates two slots of DMA memory from the
heap. A slot has the size of the largest command, at most 256 KiB. xnvme fs
allocates more slots as the thread keeps more commands in flight, up to 32
slots or 8 MiB per queue. xNVMe allocates memory of its own for the queue in
addition. When the heap cannot supply another slot, the queue continues with
the slots that it has. A heap that holds two slots for every queue is
therefore enough, even when it does not hold 32.

## Format and mount the pool

    export XNVME_FS_HOMI_ID=1
    mkfs.xnvme-fs -L pool $bdf/p2
    mkdir -p /mnt/pool
    mount.xnvme-fs $bdf/p2 /mnt/pool

`mount.xnvme-fs` starts a server for the pool in the background. The server
reads the name and the members of the pool from the superblock of the
partition and serves the view of the pool at `/mnt/pool`. `mount.xnvme-fs`
returns once the view is mounted.

To format a pool of several members, name all of them, as in
`mkfs.xnvme-fs -L pool $bdf/p2 0000:85:00.0`. To mount that pool, name any
one of its members.

A member that is striped over several drives is an md RAID 0 set. Create the
set with `mdadm --create --level=0` while the drives are bound to the
kernel's driver, and stop it with `mdadm --stop`. Name the member by its
drives joined by `+`, as in
`mkfs.xnvme-fs -L pool 0000:82:00.0+0000:83:00.0`.

## Use the pool from the engines

DuckDB uses the pool through paths of the form `xnvme-fs://POOL/NAME`:

    duckdb -c "LOAD xnvme_fs; LOAD tpch; ATTACH 'xnvme-fs://pool/tpch.db' AS t;
               SET temp_directory = 'xnvme-fs://pool/tmp'; USE t; CALL dbgen(sf=1);"

RocksDB uses the pool through its file system URI:

    db_bench --fs_uri=xnvme-fs://pool --db=/bench --benchmarks=fillrandom,readrandom

The files of both engines are listed in the view, read-only, while the
engines run:

    ls -l /mnt/pool

## Stop and return the drive to the kernel

    umount /mnt/pool
    kill %1                                   # HOMI
    echo nvme > /sys/bus/pci/devices/$bdf/driver_override
    echo $bdf > /sys/bus/pci/drivers/vfio-pci/unbind
    echo $bdf > /sys/bus/pci/drivers_probe

Unmounting the view stops the server that `mount.xnvme-fs` started. Once the
controller is bound to the kernel's `nvme` driver again, the kernel finds the
other partitions of the drive unchanged.
