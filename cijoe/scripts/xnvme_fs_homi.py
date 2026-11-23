# SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
# SPDX-License-Identifier: BSD-3-Clause
"""
xnvme_fs_homi: start or stop the HOMI server holding the pool's controllers
===========================================================================

Starting runs 'xnvme homi serve', of the xNVMe that xnvme_fs_build installed, on the members named by
xnvme_fs.guest.members, with a heap large enough for every client thread's queue, and waits until 'xnvme homi status'
reports every controller up. Stopping signals it and prints its log, so a failing run shows what HOMI saw.

On a host whose boot brings HOMI up, xnvme_fs.lab.units.prefix names where its units' binaries are installed, and
the server is the host's xnvme-homi@ID.service instead, configured by /etc/xnvme/homi-ID.conf, when that holds
every member; starting and stopping are systemctl's, and the pidfile names its main process all the same. A run on
more drives than the unit holds starts 'xnvme homi serve' itself as above, while the unit is stopped.

Retargetable: False
-------------------
"""
import errno
import logging as log
import sys
import time
from argparse import ArgumentParser
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from xnvme_fs_build import paths  # noqa: E402

# The server's name as the kernel has it: 'xnvme homi' runs xnvme-homi from xNVMe's libexec directory in its place
COMM = "xnvme-homi"


def alive(pidfile, comm):
    """A shell test that pidfile names a running process called comm. A host reset while a server ran leaves its
    pidfile in /var/tmp, and the pid it names may since be another process's"""
    return f"{{ p=$(cat {pidfile} 2>/dev/null); [ -n \"$p\" ] && [ \"$(cat /proc/$p/comm 2>/dev/null)\" = {comm} ]; }}"


def units(cijoe):
    """The host's systemd units, when xnvme_fs.lab.units.prefix names where their binaries are installed: the HOMI
    server's unit, and for each pool xnvme_fs.lab.units.boot_pools names, which the host serves outside a run, its
    unit, its configuration and the command making it anew; or None, and the workflows start HOMI and
    'xnvme fs serve' themselves. The pools' units are xnvme fs's xnvme-fs@POOL with /etc/xnvme/fs-POOL.conf, made by
    the prefix's 'xnvme fs mkfs'; xnvme_fs.lab.units.pool_unit, pool_conf and mkfs name others, with {pool} for the
    pool, where a host's boot serves its pools with another installation, mkfs taking -f, -L POOL, -H HOMI_ID and the
    members"""
    prefix = cijoe.getconf("xnvme_fs.lab.units.prefix", "")
    if not prefix:
        return None
    homi_id = int(cijoe.getconf("xnvme_fs.guest.homi_id", 1))
    unit = str(cijoe.getconf("xnvme_fs.lab.units.pool_unit", "") or "xnvme-fs@{pool}")
    conf = str(cijoe.getconf("xnvme_fs.lab.units.pool_conf", "") or "/etc/xnvme/fs-{pool}.conf")
    mkfs = str(cijoe.getconf("xnvme_fs.lab.units.mkfs", "") or "")
    pools = {}
    for pool in cijoe.getconf("xnvme_fs.lab.units.boot_pools", []):
        make = (f"{mkfs.format(pool=pool)} -f -L {pool} -H $h $ms" if mkfs else
                f"{prefix}/bin/xnvme fs mkfs --pool {pool} $(for m in $ms; do echo --member $m; done) --homi-id $h"
                " --overwrite")
        pools[pool] = {"unit": unit.format(pool=pool), "conf": conf.format(pool=pool), "mkfs": make}
    return {"prefix": prefix, "homi": f"xnvme-homi@{homi_id}", "pools": pools}


def start(unit):
    """A shell command starting unit. The units' start limit, three starts in 180 s, is for a server that fails
    and restarts; a run stops and starts them more often than that, so each start clears the count first"""
    return f"sudo systemctl reset-failed {unit} 2>/dev/null; sudo systemctl start {unit}"


def homi_running(cijoe):
    """A shell test that xnvme fs's HOMI server runs, as its unit or under its pidfile"""
    homi_id = int(cijoe.getconf("xnvme_fs.guest.homi_id", 1))
    test = alive(f"/var/tmp/xnvme-fs-homi-{homi_id}.pid", COMM)
    u = units(cijoe)
    return f"{{ systemctl -q is-active {u['homi']} || {test}; }}" if u else test


def add_args(parser: ArgumentParser):
    # A value rather than a flag: cijoe passes a step's 'with' entries as "--key value"
    parser.add_argument("--action", type=str, choices=["start", "stop"], default="start")


def main(args, cijoe):
    members = " ".join(cijoe.getconf("xnvme_fs.guest.members", []))
    homi_id = int(cijoe.getconf("xnvme_fs.guest.homi_id", 1))
    heap = int(cijoe.getconf("xnvme_fs.guest.homi_heap", 268435456))
    homi = f"{paths(cijoe)['prefix']}/bin/xnvme homi"
    # Named by the HOMI id, so on a shared host this never stops or reads another session's server
    pidfile = f"/var/tmp/xnvme-fs-homi-{homi_id}.pid"
    logfile = f"/var/tmp/xnvme-fs-homi-{homi_id}.log"
    marker = cijoe.getconf("xnvme_fs.lab.marker", "")
    u = units(cijoe)

    if args.action == "stop":
        direct = (f"if {alive(pidfile, COMM)}; then sudo kill $(cat {pidfile}); sleep 2; fi; sudo rm -f {pidfile};"
                  f" sudo cat {logfile} 2>/dev/null")
        if u:
            direct = (f"if systemctl -q is-active {u['homi']}; then sudo systemctl stop {u['homi']}; sudo rm -f {pidfile};"
                      f" journalctl -b -u {u['homi']} -n 10 --no-pager -o cat; else {direct}; fi")
        cijoe.run(f"{direct}; true")
        return 0

    # With xnvme_fs.lab.marker, only when the marker names xnvme fs: cijoe runs a step after one that failed
    if marker:
        err, _ = cijoe.run(f"grep -q '^xnvme-fs:' {marker}")
        if err:
            log.error(f"{marker} is not xnvme fs's; the host was not taken")
            return errno.EBUSY

    held = False
    if u:
        conf = f"/etc/xnvme/homi-{homi_id}.conf"
        err, _ = cijoe.run(f"for m in {members}; do grep -q \"^HOMI_DEVICES=.*$m\" {conf}"
                           f" || {{ echo \"$m is not held by {conf}; serving them without the unit\"; exit 1; }}; done")
        held = not err
    if held:
        err, _ = cijoe.run(f"{start(u['homi'])}"
                           f" && systemctl show -P MainPID {u['homi']} | sudo tee {pidfile} >/dev/null")
        if err:
            cijoe.run(f"journalctl -b -u {u['homi']} -n 30 --no-pager -o cat")
        return err

    err, _ = cijoe.run(
        f"sudo sh -c \"nohup {homi} serve {members} --be upcie --homi-id {homi_id} --host-heap-size {heap}"
        f" > {logfile} 2>&1 & echo \\$! > {pidfile}\""
    )
    if err:
        return err
    for _ in range(60):
        err, _ = cijoe.run(f"sudo {homi} status --homi-id {homi_id}")
        if not err:
            return 0
        time.sleep(2)
    log.error("homi did not report its controllers up")
    cijoe.run(f"sudo cat {logfile}")
    return errno.EAGAIN
