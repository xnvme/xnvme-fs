#!/usr/bin/env python3
# SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
#
# SPDX-License-Identifier: BSD-3-Clause
"""
Check xnvme-fs as it is built: that --help prints the usage and exits with 0,
that a command line it does not understand exits with 64, under its own name
and as the helpers mkfs.xnvme-fs and mount.xnvme-fs, and, given the front-end
of the xNVMe it was built against, that 'xnvme fs' runs it, that the overview
of 'xnvme' lists it, which takes fs.extension in its extension directory, and that
'xnvme --list-cmds' does

  cli_check.py XNVME_FS MKFS_HELPER MOUNT_HELPER [XNVME]
"""

import os
import subprocess
import sys

USAGE = "usage: xnvme fs mkfs"
EX_USAGE = 64


def run(env, *argv):
    return subprocess.run(argv, env=env, capture_output=True, text=True)


def main(fs, mkfs, mount, xnvme=None):
    failures = []

    def expect(what, result, status, stream):
        if result.returncode != status or USAGE not in getattr(result, stream):
            failures.append(
                f"{what}: exit status {result.returncode}, expected {status}"
                f" and the usage on {stream}"
            )

    expect("xnvme-fs --help", run(None, fs, "--help"), 0, "stdout")
    expect("xnvme-fs", run(None, fs), EX_USAGE, "stderr")
    expect("xnvme-fs nosuch", run(None, fs, "nosuch"), EX_USAGE, "stderr")
    # 'info' is a command of xnvme-fs, and to the helpers a member too many or a directory too few
    expect("xnvme-fs info", run(None, fs, "info"), EX_USAGE, "stderr")
    expect("mkfs.xnvme-fs", run(None, mkfs), EX_USAGE, "stderr")
    expect("mkfs.xnvme-fs --pool", run(None, mkfs, "--pool", "x"), EX_USAGE, "stderr")
    expect("mount.xnvme-fs info", run(None, mount, "info"), EX_USAGE, "stderr")

    if xnvme:
        built = os.path.dirname(os.path.abspath(fs))
        env = dict(os.environ, XNVME_EXEC_PATH=built, XNVME_EXTENSION_DIR=built)
        expect("xnvme fs --help", run(env, xnvme, "fs", "--help"), 0, "stdout")

        overview = run(env, xnvme).stdout
        if not any(line.split()[:2] == ["xnvme", "fs"] for line in overview.splitlines()):
            failures.append(f"the overview of 'xnvme' does not list 'xnvme fs':\n{overview}")
        if "fs" not in run(env, xnvme, "--list-cmds").stdout.split():
            failures.append("'xnvme --list-cmds' does not list fs")
    else:
        print("no xnvme front-end found; 'xnvme fs' is not checked")

    for failure in failures:
        print(failure)

    return 1 if failures else 0


if __name__ == "__main__":
    if len(sys.argv) not in (4, 5):
        print(__doc__.strip().splitlines()[-1].strip())
        sys.exit(64)
    sys.exit(main(*sys.argv[1:]))
