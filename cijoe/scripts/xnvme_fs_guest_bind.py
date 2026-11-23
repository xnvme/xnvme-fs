# SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
# SPDX-License-Identifier: BSD-3-Clause
"""
xnvme_fs_guest_bind: give the pool's controllers to the kernel's nvme driver, or back to vfio-pci
=================================================================================================

With HOMI stopped, binds the members named by xnvme_fs.guest.members to the driver given, so the suites after it
reach them through the kernel's backends, a namespace's block or character device.

Retargetable: False
-------------------
"""
import sys
from argparse import ArgumentParser
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from xnvme_fs_guest_prepare import bind  # noqa: E402


def add_args(parser: ArgumentParser):
    parser.add_argument("--driver", type=str, choices=["nvme", "vfio-pci"], required=True)


def main(args, cijoe):
    for bdf in cijoe.getconf("xnvme_fs.guest.members", []):
        err = bind(cijoe, bdf, args.driver)
        if err:
            return err
    if args.driver == "nvme":
        err, _ = cijoe.run("sudo udevadm settle")
        if err:
            return err
    return 0
