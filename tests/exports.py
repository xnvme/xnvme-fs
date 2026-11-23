#!/usr/bin/env python3
# SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
# SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
#
# SPDX-License-Identifier: BSD-3-Clause
"""
Fails unless libxnvme-fs exports its C interface and none of its C++ classes

A program's own class of the same name as one of the library's would otherwise
take the library's place in calls the library makes itself.

    exports.py NM LIBRARY
"""
import subprocess
import sys

LINKER = {"_init", "_fini", "__bss_start", "_edata", "_end"}


def main(nm, library):
    out = subprocess.run(
        [nm, "-D", "--defined-only", "-C", library],
        check=True,
        capture_output=True,
        text=True,
    ).stdout

    wrong = []
    for line in out.splitlines():
        name = line.split(None, 2)[-1]
        private = "xnvme_fs::" in name
        c_name = "::" not in name and " " not in name and "(" not in name
        if private or (
            c_name and not name.startswith("xnvme_fs_") and name not in LINKER
        ):
            wrong.append(name)

    for name in wrong:
        print(f"exported: {name}")
    if not any(" xnvme_fs_connect" in line for line in out.splitlines()):
        print("xnvme_fs_connect is not exported")
        return 1

    return 1 if wrong else 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__.strip().splitlines()[-1].strip())
        sys.exit(64)
    sys.exit(main(sys.argv[1], sys.argv[2]))
