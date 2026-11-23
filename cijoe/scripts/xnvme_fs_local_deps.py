# SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
# SPDX-License-Identifier: BSD-3-Clause
"""
xnvme_fs_local_deps: what this machine needs to run the guest
=============================================================

Checks for oras, the packages in xnvme_fs.local.packages and /dev/kvm, and with --install installs the
missing packages with apt. Without KVM the guest runs, but far too slowly for the suites; under WSL2, nested
virtualization in .wslconfig provides it.

Retargetable: False
-------------------
"""
import errno
import logging as log
from argparse import ArgumentParser


def add_args(parser: ArgumentParser):
    parser.add_argument("--install", type=str, choices=["yes", "no"], default="no")


def main(args, cijoe):
    packages = list(cijoe.getconf("xnvme_fs.local.packages", []))
    missing = []
    for pkg in packages:
        err, _ = cijoe.run_local(f"dpkg -s {pkg} >/dev/null 2>&1")
        if err:
            missing.append(pkg)
    if missing and args.install == "yes":
        err, _ = cijoe.run_local(
            "sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends " + " ".join(missing)
        )
        if err:
            return err
        missing = []

    problems = [f"package {p}" for p in missing]
    for tool in ("oras",):
        err, _ = cijoe.run_local(f"command -v {tool}")
        if err:
            problems.append(tool)
    err, _ = cijoe.run_local("test -c /dev/kvm && test -w /dev/kvm")
    if err:
        problems.append("/dev/kvm (none, or not writable)")
    if problems:
        log.error("missing: " + ", ".join(problems))
        return errno.ENOENT
    return 0
