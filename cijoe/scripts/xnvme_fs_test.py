# SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
# SPDX-License-Identifier: BSD-3-Clause
"""
xnvme_fs_test: one of the adapters' test suites, on the pool's controllers through HOMI
=======================================================================================

Runs a suite in the tree xnvme_fs_build built, with 'xnvme fs serve' and every engine process a HOMI client of the
server started by xnvme_fs_homi, on a pool of the members named by xnvme_fs.guest.members, or for two-pools on two
pools of one member each:

  sqllogic      the DuckDB extension's sqllogictests beside a running 'xnvme fs serve', TPC-H SF1 among them
  duckdb-suite  DuckDB's own storage tests with each test's database and temporary files on the pool
  behind        the DuckDB extension's writes behind
  bench         xnvme-fs-bench, every workload synchronous and asynchronous, briefly, with every sector checked
  rocksdb       RocksDB through the plugin: its file system and basic database tests, and db_bench
  two-engines   DuckDB and RocksDB on one pool at once, each database checked after
  two-pools     one DuckDB process with a database on each of two pools, a pool to each member
  trace         what each engine asks of the library, by operation and size, from XNVME_FS_TRACE

With --backend io_uring, io_uring_direct or io_uring_cmd, the members are reached through the kernel's nvme driver
instead, after xnvme_fs_guest_bind gave them to it: their block devices, or their character devices for io_uring_cmd,
and no HOMI. The file system's own suites, of its members, partitions, md sets, server, view and crashes, are
cijoe/tests/test_fs.py, which this script does not run.

Retargetable: False
-------------------
"""
import errno
import logging as log
import sys
from argparse import ArgumentParser
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from xnvme_fs_build import paths  # noqa: E402
from xnvme_fs_guest_prepare import blockdev, chardev  # noqa: E402

SUITES = {
    "sqllogic": 'test/with-xnvme-fs.sh ./build/release/test/unittest "test/*"',
    "duckdb-suite": "test/duckdb-suite.sh $PWD/test/configs/xnvme_fs.json",
    "behind": "test/with-xnvme-fs.sh ./build/release/extension/xnvme_fs/behind_test ci",
    "bench": "test/with-xnvme-fs.sh test/bench.sh",
    "rocksdb": "test/with-xnvme-fs.sh test/rocksdb.sh",
    "two-engines": "test/with-xnvme-fs.sh test/two-engines.sh",
    "two-pools": "test/two-pools.sh",
    "trace": "TRACE_DIR=/var/tmp/xnvme-fs-trace test/with-xnvme-fs.sh test/trace.sh",
}


def add_args(parser: ArgumentParser):
    parser.add_argument("--suite", type=str, required=True, choices=sorted(SUITES))
    parser.add_argument(
        "--backend", type=str, default="upcie", choices=["upcie", "io_uring", "io_uring_direct", "io_uring_cmd"]
    )


def main(args, cijoe):
    p = paths(cijoe)
    bdfs = cijoe.getconf("xnvme_fs.guest.members", [])
    homi_id = int(cijoe.getconf("xnvme_fs.guest.homi_id", 1))
    if args.backend == "upcie":
        env = f"TMPDIR=/var/tmp XNVME_FS_HOMI_ID={homi_id} XNVME_FS_MEMBER='{' '.join(bdfs)}'"
    else:
        # Expanded by the guest's shell, where the controllers' device names are known
        dev = chardev if args.backend == "io_uring_cmd" else blockdev
        members = " ".join(dev(bdf) for bdf in bdfs)
        env = f"TMPDIR=/var/tmp XNVME_FS_BACKEND={args.backend} XNVME_FS_HOMI_ID=0 XNVME_FS_MEMBER=\"{members}\""
    cmd = SUITES[args.suite].replace("'", "'\\''")
    err, _ = cijoe.run(f"cd {p['adapters']} && sudo env PATH={p['prefix']}/bin:$PATH {env} sh -c '{cmd}'")
    if err:
        log.error(f"suite {args.suite} failed")
        return err or errno.EIO
    return 0
