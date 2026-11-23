# SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
# SPDX-License-Identifier: BSD-3-Clause
"""
xnvme_fs_guest_start: boot the guest with emulated NVMe controllers
===================================================================

Plain NVM controllers, 4 KiB blocks, one namespace each and each behind its own PCIe root port, so the guest
enumerates them as 0000:01:00.0, 0000:02:00.0, and so on. Their backing images are raw files next to the guest
image, created on first use; QEMU opens them with O_DIRECT, so a flush in the guest reaches the host's disk.

Retargetable: False
-------------------
"""
import errno
import logging as log
from argparse import ArgumentParser
from pathlib import Path

from cijoe.qemu.wrapper import Guest


def add_args(parser: ArgumentParser):
    parser.add_argument("--guest_name", type=str, default=None)
    parser.add_argument("--timeout", type=int, default=300, help="Seconds to wait for the login prompt.")


def controller_args(index, image):
    port = f"rp{index}"
    ctrl = f"nvme{index}"
    drive = f"{ctrl}n1"
    return [
        "-device",
        f"pcie-root-port,id={port},chassis={index + 1},slot={index + 1}",
        "-device",
        f"nvme,id={ctrl},serial=xnvmefs{index},bus={port},mdts=7,ioeventfd=on",
        "-drive",
        f"id={drive},file={image},format=raw,if=none,discard=on,detect-zeroes=unmap,cache=none,aio=threads",
        "-device",
        f"nvme-ns,id={drive},drive={drive},bus={ctrl},nsid=1,logical_block_size=4096,physical_block_size=4096",
    ]


def main(args, cijoe):
    guest_name = args.guest_name or cijoe.getconf("qemu.default_guest")
    guest = Guest(cijoe, cijoe.config, guest_name)

    controllers = int(cijoe.getconf("xnvme_fs.guest.controllers", 1))
    size = cijoe.getconf("xnvme_fs.guest.controller_size", "8G")
    extra = []
    for index in range(controllers):
        image = Path(guest.guest_path) / f"nvme{index}n1.img"
        if not image.exists():
            guest.image_create(str(image), "raw", size)
        extra += controller_args(index, image)

    err = guest.start(extra_args=extra)
    if err:
        log.error(f"guest.start(): err({err})")
        return err
    if not guest.is_up(timeout=args.timeout):
        log.error("the guest did not reach its login prompt")
        return errno.EAGAIN
    return 0
