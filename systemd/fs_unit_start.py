#!/usr/bin/env python3
# SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
# SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
#
# SPDX-License-Identifier: BSD-3-Clause
"""
Check the command line xnvme-fs@.service starts 'xnvme fs serve' with

The unit's ExecStart is a shell script that builds the command line from the
variables of the instance's configuration. This runs it as systemd would, '$$'
as '$' and '%i' as the instance, with a stand-in for the installed program that
prints its arguments, for a pool on a HOMI server and for one on a kernel
backend, which has no HOMI id.

  fs_unit_start.py UNIT LIBEXECDIR
"""

import re
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

INSTANCE = "ci"

CASES = [
    (
        "members held by a HOMI server",
        {"XNVME_FS_MEMBER": "0000:03:00.0 0000:04:00.0", "XNVME_FS_HOMI_ID": "7"},
        "serve --pool ci --member 0000:03:00.0 --member 0000:04:00.0 --homi-id 7"
        " --socket /run/xnvme-fs/ci.sock",
    ),
    (
        "members held by the kernel",
        {"XNVME_FS_MEMBER": "/dev/nvme0n1", "XNVME_FS_OPTS": "--trace"},
        "serve --pool ci --member /dev/nvme0n1 --socket /run/xnvme-fs/ci.sock --trace",
    ),
]


def main(unit, libexecdir):
    text = Path(unit).read_text()
    start = re.search(r"^ExecStart=(.*)$", text, re.M)
    if not start:
        print(f"{unit}: no ExecStart")
        return 1

    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        standin = Path(tmp) / "xnvme-fs"
        standin.write_text('#!/bin/sh\necho "$*"\n')
        standin.chmod(0o755)

        line = start.group(1).replace(f"{libexecdir}/xnvme-fs", str(standin))
        line = line.replace("$$", "$").replace("%i", INSTANCE)

        for name, env, expected in CASES:
            result = subprocess.run(
                shlex.split(line), env=env, capture_output=True, text=True
            )
            got = result.stdout.strip()
            if result.returncode or got != expected:
                failures += 1
                print(f"{name}: exit status {result.returncode}")
                print(f"  expected: {expected}")
                print(f"  got:      {got}")
                print(result.stderr.strip())

    return 1 if failures else 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__.strip().splitlines()[-1].strip())
        sys.exit(64)
    sys.exit(main(*sys.argv[1:]))
