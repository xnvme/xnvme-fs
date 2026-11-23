# SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
#
# SPDX-License-Identifier: BSD-3-Clause
"""
The backend configurations of xNVMe on Linux, from xNVMe's cijoe/tests/xnvme_be_combinations.py at 088759e61
"""


def get_backend_configurations():
    """Return all sensible backend configurations matching driver presets"""

    combos = {"linux": []}

    combos["linux"] = [
        # NVMe char device
        {
            "be": ["io_uring_cmd"],
            "async": ["io_uring_cmd"],
            "sync": ["nvme"],
            "admin": ["nvme"],
            "mem": ["posix", "hugepage"],
            "label": ["cdev"],
        },
        # NVMe block device
        {
            "be": ["io_uring"],
            "async": ["io_uring"],
            "sync": ["nvme"],
            "admin": ["nvme"],
            "mem": ["posix", "hugepage"],
            "label": ["bdev"],
        },
        {
            "be": ["libaio"],
            "async": ["libaio"],
            "sync": ["nvme"],
            "admin": ["nvme"],
            "mem": ["posix", "hugepage"],
            "label": ["bdev"],
        },
        {
            "be": ["thrpool"],
            "async": ["thrpool"],
            "sync": ["nvme"],
            "admin": ["nvme"],
            "mem": ["posix", "hugepage"],
            "label": ["bdev"],
        },
        {
            "be": ["emu"],
            "async": ["emu"],
            "sync": ["nvme"],
            "admin": ["nvme"],
            "mem": ["posix", "hugepage"],
            "label": ["bdev"],
        },
        # Generic block device
        {
            "be": ["io_uring_bdev"],
            "async": ["io_uring"],
            "sync": ["block"],
            "admin": ["block"],
            "mem": ["posix", "hugepage"],
            "label": ["bdev"],
        },
        {
            "be": ["libaio_bdev"],
            "async": ["libaio"],
            "sync": ["block"],
            "admin": ["block"],
            "mem": ["posix", "hugepage"],
            "label": ["bdev"],
        },
        {
            "be": ["thrpool_bdev"],
            "async": ["thrpool"],
            "sync": ["block"],
            "admin": ["block"],
            "mem": ["posix", "hugepage"],
            "label": ["bdev"],
        },
        {
            "be": ["emu_bdev"],
            "async": ["emu"],
            "sync": ["block"],
            "admin": ["block"],
            "mem": ["posix", "hugepage"],
            "label": ["bdev"],
        },
        # File
        {
            "be": ["io_uring_file"],
            "async": ["io_uring"],
            "sync": ["psync"],
            "admin": ["file_as_ns"],
            "mem": ["posix", "hugepage"],
            "label": ["file"],
        },
        {
            "be": ["libaio_file"],
            "async": ["libaio"],
            "sync": ["psync"],
            "admin": ["file_as_ns"],
            "mem": ["posix", "hugepage"],
            "label": ["file"],
        },
        {
            "be": ["thrpool_file"],
            "async": ["thrpool"],
            "sync": ["psync"],
            "admin": ["file_as_ns"],
            "mem": ["posix", "hugepage"],
            "label": ["file"],
        },
        {
            "be": ["emu_file"],
            "async": ["emu"],
            "sync": ["psync"],
            "admin": ["file_as_ns"],
            "mem": ["posix", "hugepage"],
            "label": ["file"],
        },
        # User-space NVMe-driver
        {
            "be": ["libvfn"],
            "async": ["libvfn"],
            "sync": ["libvfn"],
            "admin": ["libvfn"],
            "mem": ["libvfn"],
            "label": ["pcie"],
        },
        {
            "be": ["spdk"],
            "async": ["spdk"],
            "sync": ["spdk"],
            "admin": ["spdk"],
            "mem": ["spdk"],
            "label": ["pcie"],
            "homi": True,
        },
        {
            "be": ["spdk"],
            "async": ["spdk"],
            "sync": ["spdk"],
            "admin": ["spdk"],
            "mem": ["spdk"],
            "label": ["fabrics"],
        },
        {
            "be": ["spdk"],
            "async": ["spdk"],
            "sync": ["spdk"],
            "admin": ["spdk"],
            "mem": ["spdk"],
            "label": ["rdma"],
        },
        {
            "be": ["upcie"],
            "mem": ["upcie"],
            "async": ["upcie"],
            "sync": ["upcie"],
            "admin": ["upcie"],
            "label": ["pcie"],
            "homi": True,
        },
        {
            # The commands are uPCIe's; the allocator and the queue setup are
            # the GPU's.
            "be": ["upcie-cuda"],
            "mem": ["upcie-cuda"],
            "async": ["upcie-cuda"],
            "sync": ["upcie"],
            "admin": ["upcie"],
            "label": ["cuda"],
            "homi": True,
        },
        {
            # The commands are uPCIe's; the allocator and the queue setup are
            # the GPU's.
            "be": ["upcie-hip"],
            "mem": ["upcie-hip"],
            "async": ["upcie-hip"],
            "sync": ["upcie"],
            "admin": ["upcie"],
            "label": ["hip"],
            "homi": True,
        },
        # The block device, with IO buffers in GPU memory registered with
        # io_uring as dma-bufs
        {
            "be": ["io_uring_dmabuf"],
            "async": ["io_uring_dmabuf"],
            "sync": ["nvme"],
            "admin": ["nvme"],
            "mem": ["io_uring_dmabuf"],
            "label": ["dmabuf"],
        },
        # Ramdisk
        {
            "be": ["ramdisk_emu"],
            "async": ["emu"],
            "sync": ["ramdisk"],
            "admin": ["ramdisk"],
            "mem": ["posix", "hugepage"],
            "label": ["bdev", "ramdisk"],
        },
        {
            "be": ["ramdisk_thrpool"],
            "async": ["thrpool"],
            "sync": ["ramdisk"],
            "admin": ["ramdisk"],
            "mem": ["posix", "hugepage"],
            "label": ["bdev", "ramdisk"],
        },
    ]

    return combos
