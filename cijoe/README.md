<!--
SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>

SPDX-License-Identifier: CC-BY-4.0
-->

# Builds and tests as cijoe workflows

The builds and the test suites in this directory are
[cijoe](https://cijoe.readthedocs.io) workflows. The report of a cijoe run
shows the system that the run used and every command that ran. `configs/`
holds the configurations, `workflows/` the workflows, and `scripts/` the
steps. Each step is a cijoe script with the entry point `main(args, cijoe)`.

## Building

The workflow `xnvme-fs-build` builds xNVMe, xnvme fs and the adapters from
source with one step, `xnvme_fs_build`. The step copies two checkouts from
the machine that runs cijoe to the host, in the state they are checked out:

* xNVMe, from the checkout that `XNVME_FS_XNVME` names, by default
  `~/git/xnvme`. The checkout must be on the branch `api-pass` of
  github.com/safl/xnvme, which has the commit "feat(cli): let other projects
  extend the commands of xnvme".
* xnvme fs with its adapters, from this checkout with its submodules.

Initialize the submodules with the command below. The submodules need their
tags `v2.0.0` and `v11.8.0`, because the build takes the version of DuckDB
from `git describe`.

    git submodule update --init adapters/duckdb adapters/extension-ci-tools adapters/third_party/rocksdb

On the host, the step does the following:

1. It builds xNVMe and installs it into a prefix in the directory that
   `xnvme_fs.tree` names.
2. It builds xnvme fs against that xNVMe and installs it into the same
   prefix, where `xnvme fs` runs `xnvme-fs` as an extension.
3. It builds the adapters against that prefix, with the make targets that
   `xnvme_fs.build.targets` names. With `configs/local.toml` these are DuckDB
   with the extension, and RocksDB's `db_bench` and tests with the plugin.

With the target `rocksdb-stock`, which `configs/local.toml` does not name,
the step also builds `db_bench` from RocksDB's source without the plugin. The
step fails when that `db_bench` links libxnvme-fs.

The file `artifacts/build.txt` of the run records both commits, the number of
tracked files that differ from each commit, and the versions that were built.
The file `COMMIT` in the tree holds the two commits. When the host already
holds a tree built from the same commits, the same uncommitted changes and
the same targets, the step keeps that tree and builds nothing.

The host needs meson, ninja, cmake, a C++ compiler, pkg-config, curl, and the
headers of liburing, libaio and libnuma. When the host has no gflags, the
step builds gflags v2.2.2 into the tree.

## The workflow xnvme-fs-guest

The workflow `xnvme-fs-guest` runs the test suites of the adapters through
uPCIe and HOMI without a physical drive. It boots a QEMU guest from xNVMe's
Ubuntu 26.04 nosi image, with an IOMMU and two emulated NVMe controllers. The
members of the pool are the two controllers. They are bound to vfio-pci inside
the guest and served by `xnvme homi serve`. `xnvme fs serve`, DuckDB and
RocksDB are clients of that HOMI server.

The prefix of xNVMe, the binaries and the test data come from a tree that
`xnvme-fs-build` built on the host. The machine needs KVM, QEMU, oras, and
local storage for the tree and the images:

```
XNVME_FS_TREE=/mnt/xnvme-fs/tree cijoe workflows/xnvme-fs-build.yaml -c configs/local.toml -o cijoe-output-build --skip-report
XNVME_FS_GUEST_ROOT=/mnt/xnvme-fs/guest XNVME_FS_TREE=/mnt/xnvme-fs/tree \
  cijoe workflows/xnvme-fs-guest.yaml -c configs/qemu-guest.toml -o cijoe-output --skip-report
```

The workflow `xnvme-fs-trace` runs the trace suite alone in the same guest
and builds the table of I/O patterns from the trace.

Neither workflow runs the test suites of the file system itself. Those are
`tests/test_fs.py`, described in the next section.

## tests/test_fs.py

`tests/test_fs.py` holds the test suites of the file system as pytest cases
that take cijoe's `cijoe` fixture. The cases cover the reads and writes of
the members, the C interface, `xnvme fs cp`, the command line, one client per
file, the FUSE view, crashes, statistics, traces, GPT partitions and md RAID
0 sets.

The cases overwrite the controllers that `configs/test-fs.toml` names. Run
them only on a machine or a guest whose controllers may be overwritten. Run
them as root, with `xnvme` and its extension `xnvme-fs` in `PATH`, and with
xnvme fs built with `-Dtests=true`, which installs the programs that
`xnvme test fs-<name>` runs.

```
pytest --config configs/test-fs.toml --output /tmp/fs-homi --homi-id 42 -k pcie tests/test_fs.py
pytest --config configs/test-fs.toml --output /tmp/fs-kernel -k test_stripe_kernel tests/test_fs.py
```

The first command starts `xnvme homi serve` on the controllers labelled
`pcie`, with the HOMI id given, and runs the cases as clients of that server.
The second command runs the case `test_stripe_kernel` on the kernel's
backends.

`tests/conftest.py` and `tests/xnvme_be_combinations.py` come from xNVMe,
reduced to what `test_fs.py` uses. `configs/test-fs.toml` holds the devices
of xNVMe's QEMU guest that carry the label `fs`.

No workflow and no CI job runs these cases. The guest of `xnvme-fs-guest`
has two controllers, fewer than `configs/test-fs.toml` names, and its root
account is locked.
