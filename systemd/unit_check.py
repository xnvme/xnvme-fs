#!/usr/bin/env python3
# SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
# SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
#
# SPDX-License-Identifier: BSD-3-Clause
"""
Check a systemd unit as meson configured it: no @variable@ is left in it, its
EnvironmentFile lies under the configuration directory, its ExecStart runs
the installed program with the given subcommand, and, where systemd-analyze is
installed, 'systemd-analyze verify' accepts an instance of the unit

Exits with 77, which meson reports as a skip, when the unit passes the other
checks and systemd-analyze is installed but cannot verify: in a container or a
build environment without a running systemd, it fails before it reads the unit.

  unit_check.py UNIT INSTANCE LIBEXECDIR SYSCONFDIR PROGRAM SUBCOMMAND
"""

import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SKIP = 77


def verify(unit, instance, installed):
    """
    Run 'systemd-analyze verify' on an instance of the unit; return what it says
    of it, or None when it could not verify

    verify fails for a program that does not exist, and nothing is installed
    when this runs, so the instance names a stand-in in place of 'installed'.

    As the system manager, which the unit is for, verify needs /run/systemd.
    Where that fails, it is run as the user manager, which reads the same keys.
    """

    with tempfile.TemporaryDirectory() as tmp:
        standin = Path(tmp) / Path(installed).name
        standin.write_text("#!/bin/sh\n")
        standin.chmod(0o755)

        copy = Path(tmp) / unit.name.replace("@.", f"@{instance}.")
        copy.write_text(unit.read_text().replace(installed, str(standin)))

        for manager in [], ["--user"]:
            result = subprocess.run(
                ["systemd-analyze", *manager, "verify", "--man=no", str(copy)],
                capture_output=True,
                text=True,
            )

            # verify loads the units of the machine as well, and reports on those too
            said = [line for line in result.stderr.splitlines() if copy.name in line]
            if said or not result.returncode:
                return said

            # It failed and said nothing of the unit, so it did not get to read it
            print(f"systemd-analyze {' '.join(manager + ['verify'])} could not run:")
            print(result.stderr.strip())

        return None


def main(unit, instance, libexecdir, sysconfdir, program, subcommand):
    unit = Path(unit)
    text = unit.read_text()
    failures = []

    left = re.findall(r"@[a-z_]+@", text)
    if left:
        failures.append(f"left unsubstituted: {sorted(set(left))}")

    if not re.search(rf"^EnvironmentFile={re.escape(sysconfdir)}/xnvme/", text, re.M):
        failures.append(f"no EnvironmentFile under {sysconfdir}/xnvme/")

    installed = f"{libexecdir}/{program}"
    start = re.search(r"^ExecStart=(.*)$", text, re.M)
    if not start or f"{installed} {subcommand}" not in start.group(1):
        failures.append(f"ExecStart does not run '{installed} {subcommand}'")

    verified = True
    if shutil.which("systemd-analyze"):
        said = verify(unit, instance, installed)
        verified = said is not None
        for line in said or []:
            failures.append(f"systemd-analyze verify: {line}")
    else:
        print("systemd-analyze is not installed; the unit is not verified")

    for failure in failures:
        print(f"{unit.name}: {failure}")

    if failures:
        return 1

    return 0 if verified else SKIP


if __name__ == "__main__":
    sys.exit(main(*sys.argv[1:]))
