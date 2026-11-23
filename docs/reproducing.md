<!--
SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>

SPDX-License-Identifier: CC-BY-4.0
-->

# Reproducing the builds and the tests

The builds of xNVMe, xnvme fs and the adapters, and their tests, are cijoe
workflows under `cijoe/`. A cijoe run records the system that it ran on, and
every command with its output.

Run the commands below from `cijoe/`, in a checkout of this repository with
its submodules:

    git submodule update --init adapters/duckdb adapters/extension-ci-tools adapters/third_party/rocksdb

The commands also need a checkout of the branch `api-pass` of
github.com/safl/xnvme, which has the commit "feat(cli): let other projects
extend the commands of xnvme". `XNVME_FS_XNVME` names that checkout when it
is not at `~/git/xnvme`.

## What you need

Building and checking correctness need one Linux machine on x86-64 with KVM,
docker, and about 30 GB of local disk. The build runs in xNVMe's image with
its dependencies for Ubuntu 26.04. The tests run in the host container of
xnvme-devenv, as they do in CI. The tests need no NVMe drive and write to
none: they run inside the QEMU guest of xnvme-devenv, whose NVMe controllers
are emulated.

## 1. Build

    docker run --rm -v $PWD/..:$PWD/.. -v $HOME/git/xnvme:$HOME/git/xnvme -v $HOME/.cache/xnvme-fs:$HOME/.cache/xnvme-fs \
      -w $PWD -e XNVME_FS_TREE=$HOME/.cache/xnvme-fs/tree -e XNVME_FS_XNVME=$HOME/git/xnvme \
      ghcr.io/xnvme/xnvme/distro-deps-ubuntu-resolute:main bash -euc '
        git config --global --add safe.directory "*" && ../adapters/scripts/ci-deps.sh && pipx install cijoe==0.9.70
        ~/.local/bin/cijoe workflows/xnvme-fs-build.yaml -c configs/local.toml -o cijoe-output-build --skip-report'

`adapters/scripts/ci-deps.sh` installs the packages that the adapters need
in addition to the dependencies of xNVMe. The workflow then does the
following:

1. It copies the two checkouts to `XNVME_FS_TREE`.
2. It builds xNVMe and installs it into the prefix of the tree.
3. It builds xnvme fs from this checkout and installs it into the same
   prefix.
4. It builds, against that prefix, the DuckDB extension with its tests, the
   benchmark, and RocksDB with the plugin.

The workflow records the commits of both checkouts in `artifacts/build.txt`.
When `XNVME_FS_TREE` already holds a tree built from the same commits, the
workflow keeps that tree and builds nothing.

## 2. Check correctness

    docker run --rm --privileged -v $PWD/..:$PWD/.. -v $HOME/.cache/xnvme-fs:$HOME/.cache/xnvme-fs -w $PWD \
      -e XNVME_FS_TREE=$HOME/.cache/xnvme-fs/tree -e XNVME_FS_GUEST_ROOT=$HOME/.cache/xnvme-fs/guest \
      ghcr.io/xnvme/xnvme-devenv/host:v0.15.0 \
      cijoe workflows/xnvme-fs-guest.yaml -c configs/qemu-guest.toml -o cijoe-output-guest --skip-report

The workflow boots the Ubuntu 26.10 guest of xnvme-devenv with an IOMMU and
two emulated NVMe controllers. Inside the guest it binds the controllers to
vfio-pci and starts `xnvme homi serve` on them. It then runs each test suite
as a step of its own, on a pool of both controllers that `xnvme fs serve`
serves. The suites are:

* DuckDB's sqllogictests, with TPC-H at scale factor 1
* DuckDB's storage tests
* the write-behind test of the extension
* the benchmark, which verifies every sector
* RocksDB's file system tests and basic database tests, and `db_bench`
* DuckDB and RocksDB on one pool at the same time
* the trace of the operations that each engine requests from the file
  system, with a table of the I/O patterns in
  `cijoe-output-guest/artifacts/io-patterns.md`

The workflow then binds the controllers to the kernel's `nvme` driver and
runs four of the suites again with each of the kernel backends `io_uring`,
`io_uring_direct` and `io_uring_cmd`: the write-behind test, the benchmark,
the RocksDB suite, and the two engines on one pool. A suite that fails does
not stop the suites after it.

The workflow does not run the test suites of the file system itself, which
are `cijoe/tests/test_fs.py`. `cijoe/README.md` describes how to run them.
