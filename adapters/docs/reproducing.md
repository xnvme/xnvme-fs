<!--
SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>

SPDX-License-Identifier: CC-BY-4.0
-->

# Reproducing the builds and the tests

Building xNVMe, xnvme fs and the adapters, and testing them, runs as cijoe
workflows under `cijoe/`. A workflow records the system it ran on and keeps
every command and its output. Run the commands below from `cijoe/` in a
checkout with its submodules (`git submodule update --init adapters/duckdb
adapters/extension-ci-tools adapters/third_party/rocksdb`), beside a checkout
of the branch `api-pass` of github.com/safl/xnvme, which has the commit that
lets xNVMe run extensions; `XNVME_FS_XNVME` names the checkout when it is not
`~/git/xnvme`.

## What you need

For building and for checking correctness, one Linux machine on x86-64 with
KVM, QEMU (`qemu-system-x86`, `qemu-utils`), oras, cijoe 0.9.70
(`pipx install cijoe==0.9.70`), what `adapters/scripts/ci-deps.sh` installs, and
about 30 GB of local disk. No NVMe drive is needed and none is touched: the
tests run inside a QEMU guest whose controllers are emulated.

## 1. Build

    XNVME_FS_TREE=$HOME/.cache/xnvme-fs-tree cijoe workflows/xnvme-fs-build.yaml -c configs/local.toml \
      -o cijoe-output-build --skip-report

The first step checks for what the guest needs; with `install: yes` in its
`with:` it installs the missing packages. The second copies the two checkouts
to `XNVME_FS_TREE`, builds xNVMe and installs it into the tree's prefix, builds xnvme fs
from this checkout into the same prefix, and builds against that prefix the DuckDB extension with its
tests, the benchmark and RocksDB with the plugin. It records the commits of
both checkouts in `artifacts/build.txt`. A tree built from the same commits is
kept, and nothing is built again.

## 2. Check correctness

    XNVME_FS_GUEST_ROOT=$HOME/.cache/xnvme-fs-guest XNVME_FS_TREE=$HOME/.cache/xnvme-fs-tree \
      cijoe workflows/xnvme-fs-guest.yaml -c configs/qemu-guest.toml -o cijoe-output-guest --skip-report

This boots an Ubuntu 26.04 guest with an IOMMU and two emulated NVMe controllers,
binds them to vfio-pci inside it, starts `xnvme homi serve` on them and runs
each suite as a step on a pool of both, served by `xnvme fs serve`: DuckDB's
sqllogictests with TPC-H SF1, DuckDB's storage tests, the extension's writes
behind, the benchmark with every sector verified, RocksDB's file system and
basic database tests and `db_bench`, DuckDB and RocksDB on one pool at once, and
the trace of what each engine asks of the file system, with its table of I/O
patterns in `cijoe-output-guest/artifacts/io-patterns.md`. Four of them run
again through each kernel backend. A failed suite does not stop the ones after
it. The file system's own suites are
`cijoe/tests/test_fs.py`, which this workflow does not run; `cijoe/README.md`
says how they run.
