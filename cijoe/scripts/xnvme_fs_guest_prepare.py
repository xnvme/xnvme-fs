# SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
# SPDX-License-Identifier: BSD-3-Clause
"""
xnvme_fs_guest_prepare: make the guest ready for HOMI and copy the build in
===========================================================================

Waits for ssh, installs the libraries the binaries link, reserves hugepages for HOMI's DMA heap, checks that the
guest has an IOMMU, binds the pool's members to vfio-pci, and copies into the guest, at the paths they have on this
host, what the suites need of the tree xnvme_fs_build built at xnvme_fs.tree: the install prefix of xNVMe and
xnvme fs, and the adapters' binaries and test data, since DuckDB's test runner changes into the directory it was compiled in.

Retargetable: False
-------------------
"""
import errno
import logging as log
import sys
import time
from pathlib import Path

from cijoe.qemu.wrapper import Guest

sys.path.insert(0, str(Path(__file__).parent))
from xnvme_fs_build import paths  # noqa: E402

# What the suites need from the adapters' built tree
ARTIFACTS = [
    "test",
    "build/release/duckdb",
    "build/release/test/unittest",
    # unittest loads it by its RUNPATH, the same path in the guest
    "build/release/src/libduckdb.so",
    "build/release/extension/xnvme_fs/behind_test",
    "build/release/extension/xnvme_fs/xnvme-fs-bench",
    "build/rocksdb/db_bench",
    "build/rocksdb/env_basic_test",
    "build/rocksdb/db_basic_test",
    "build/rocksdb/xnvme_fs_async_read_test",
    "duckdb/test",
    "duckdb/data",
    "duckdb/extension/tpch/dbgen/answers",
]


def blockdev(bdf):
    """The kernel's block device of the controller 'bdf', as a shell expression."""
    return (
        f"/dev/$(ls /sys/bus/pci/devices/{bdf}/nvme/*/ | grep -m1 -E '^nvme[0-9]+(c[0-9]+)?n1$'"
        " | sed -E 's/c[0-9]+n/n/')"
    )


def chardev(bdf):
    """The kernel's NVMe character device of the controller 'bdf''s first namespace, as a shell expression."""
    return f"/dev/$(ls /sys/bus/pci/devices/{bdf}/nvme/*/ | grep -m1 -E '^ng[0-9]+n1$')"


def bind(cijoe, bdf, driver):
    """Binds the controller 'bdf' to 'driver', vfio-pci or nvme."""
    dev = f"/sys/bus/pci/devices/{bdf}"
    err, _ = cijoe.run(
        f"echo {driver} | sudo tee {dev}/driver_override"
        f" && if [ -e {dev}/driver ]; then echo {bdf} | sudo tee {dev}/driver/unbind; fi"
        f" && echo {bdf} | sudo tee /sys/bus/pci/drivers_probe"
        f" && test \"$(basename $(readlink {dev}/driver))\" = {driver}"
    )
    if err:
        log.error(f"could not bind {bdf} to {driver}")
    return err


def wait_for_ssh(cijoe, timeout=300):
    began = time.time()
    while time.time() - began < timeout:
        err, _ = cijoe.run("true")
        if not err:
            return True
        time.sleep(5)
    return False


def main(args, cijoe):
    p = paths(cijoe)
    adapters = Path(p["adapters"])
    members = cijoe.getconf("xnvme_fs.guest.members", [])
    hugepages = int(cijoe.getconf("xnvme_fs.guest.hugepages", 256))
    if not (adapters / "build/release/duckdb").exists() or not (Path(p["prefix"]) / "bin/xnvme").exists():
        log.error(f"xnvme_fs.tree({p['tree']}) holds no build; run workflows/xnvme-fs-build.yaml with XNVME_FS_TREE")
        return errno.EINVAL
    if not wait_for_ssh(cijoe):
        log.error("no ssh into the guest")
        return errno.EAGAIN

    steps = [
        "sudo apt-get -qq update",
        "sudo DEBIAN_FRONTEND=noninteractive apt-get -qq install -y --no-install-recommends"
        " liburing2 libaio1t64 libnuma1 libgflags2.2 gdb pciutils",
        f"echo {hugepages} | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages",
        "grep -H . /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages",
        # vfio-pci needs an IOMMU group for each device; an empty list means the guest has none.
        "test -n \"$(ls /sys/kernel/iommu_groups)\"",
        "sudo modprobe vfio-pci",
        "lspci -D -nn | grep -i 'non-volatile'",
    ]
    for cmd in steps:
        err, _ = cijoe.run(cmd)
        if err:
            log.error(f"failed: {cmd}")
            return err

    for bdf in members:
        err = bind(cijoe, bdf, "vfio-pci")
        if err:
            return err

    guest = Guest(cijoe, cijoe.config, cijoe.getconf("qemu.default_guest"))
    tarball = Path(guest.guest_path) / "xnvme-fs-artifacts.tgz"
    rel = adapters.relative_to("/")
    listed = " ".join([str(Path(p["prefix"]).relative_to("/"))] + [str(rel / a) for a in ARTIFACTS])
    err, _ = cijoe.run_local(f"tar -czf {tarball} -C / {listed}")
    if err:
        return err
    ok = cijoe.put(str(tarball), "/tmp/xnvme-fs-artifacts.tgz")
    tarball.unlink()
    if not ok:
        log.error("could not copy the artifacts into the guest")
        return errno.EIO
    # --no-same-owner: xnvme runs an xnvme-<name> program only from a directory owned by root or by the user running
    # xnvme, and the archive records the uid of the user who built it
    err, _ = cijoe.run(
        "sudo tar -xzf /tmp/xnvme-fs-artifacts.tgz --no-same-owner -C / && rm /tmp/xnvme-fs-artifacts.tgz"
    )
    return err
