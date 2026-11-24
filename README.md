<!--
SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>

SPDX-License-Identifier: CC-BY-4.0
-->

# xnvme fs

xnvme fs is a user-space file system on NVMe drives, built on xNVMe. This
repository holds the file system, which is a library, a command-line program
and a systemd unit. It also holds a DuckDB extension and a RocksDB plugin
that store the files of those engines on xnvme fs, the documentation, and the
cijoe workflows that build and test all of them.

The drives of a file system form a pool. A pool is a set of members, and the
bytes of each file are stored in extents that are striped over the members.
`xnvme fs serve` serves the metadata of one pool on a Unix socket. The file
data does not pass through that server. Each client process reads and writes
the extents of its files on its own NVMe queues, either through xNVMe's uPCIe
backend as a client of a HOMI server, or through the Linux kernel's NVMe
driver with io_uring.

xnvme fs depends on xNVMe and extends the `xnvme` command line. The build
installs the program `xnvme-fs` into the directory that `xnvme` runs its
commands from, so `xnvme fs` runs `xnvme-fs`. The build also installs
`fs.extension` into xNVMe's extension directory, so the overview that `xnvme`
prints lists `xnvme fs`.

| Path | Content |
|---|---|
| `include/`, `lib/` | The library libxnvme-fs and its C interface `libxnvme_fs.h`. |
| `tools/` | The program `xnvme-fs` with the commands `mkfs`, `serve`, `info` and `cp`, and the helpers `mkfs.xnvme-fs` and `mount.xnvme-fs`. |
| `tests/` | The test programs, which run as `xnvme test fs-<name>`. |
| `systemd/` | The unit `xnvme-fs@.service`, one instance per pool, and an example configuration. |
| `docs/` | The documentation: the concepts, the C API, the tools, the adapters and the design. It is a Sphinx project in MyST Markdown, and `make -C docs html` builds it. |
| `adapters/` | The DuckDB extension, the RocksDB plugin, the C++ header that both use, and their tests. `adapters/README.md` describes how to build and run them. |
| `cijoe/` | The cijoe workflows that build xnvme fs and run the adapters' test suites, and `tests/test_fs.py`, the test suites of the file system that need NVMe controllers. |

## Building and installing

xnvme fs is a meson project in C and C++17, for Linux. The build finds xNVMe
by pkg-config and takes uFuse v0.4 as a meson wrap.

xnvme fs needs xNVMe at the commit that `XNVME_REF` in
`.github/workflows/verify-emulated.yml` pins, a commit of the branch `wip` of
github.com/safl/xnvme. No release of xNVMe has that commit yet. libxnvme-fs
does not compile against an xNVMe without its spec headers, its command preps
and `xnvme_buf_register_dmabuf()`.

With xNVMe installed in `$PREFIX`:

    export PKG_CONFIG_PATH=$(dirname $(find $PREFIX -name xnvme.pc))
    meson setup builddir --prefix $PREFIX
    meson compile -C builddir
    meson test -C builddir
    meson install -C builddir
    xnvme fs --help

`meson install` installs `xnvme-fs` and the test programs into the directory
that `pkg-config --variable=libexecdir xnvme` prints, whatever `--prefix` is.
It installs libxnvme-fs, `libxnvme_fs.h`, `xnvme-fs.pc`, the helpers and the
systemd unit under `--prefix`. The option `-Dtests=false` disables the test
programs. The option `-Dsystemd_unitdir=DIR` sets the directory of the unit
for a system where systemd's pkg-config file is not installed.

`meson test` runs the tests that need no NVMe controller. They check the
symbols that libxnvme-fs exports, the systemd unit, the command line under
`xnvme`, and the C++ layer that the adapters use.
`adapters/scripts/generate-cpp.sh` generates that layer from `libxnvme_fs.h`
with yace. `cijoe/README.md` describes how to run the test suites that need
controllers.

## Licenses

The file system and its cijoe workflows are licensed BSD-3-Clause, the
adapters MIT, and the documentation CC-BY-4.0. `REUSE.toml` and the header of
each file state the license of the file. The license texts are in
`LICENSES/`.

## Releasing

A release is made by merging a release pull request. The last commit of a
release pull request has the subject `ver: bump to v<version>`, sets that
version in `meson.build`, and adds a section `## <version>` to
`CHANGELOG.md`.

On a release pull request, the job `gates` of `.github/workflows/verify.yml`
fails unless both files name the version and the branch contains every commit
of main. The job `ci-result` fails unless every job of the run passed. Merge
the pull request by rebase once `ci-result` has passed.

The workflow `publish-release` then runs on main. It checks that the pull
request had the same tree as main and that the `ci-result` job of the pull
request passed. It tags the commit `v<version>` and creates the GitHub
Release, with the section of `CHANGELOG.md` as the release notes. It attaches
the source archive, the documentation and the test results from the run of
the pull request. The workflow uploads nothing to any other service.
