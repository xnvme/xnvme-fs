<!--
SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>

SPDX-License-Identifier: CC-BY-4.0
-->

# xnvme fs adapters

This directory holds the DuckDB extension and the RocksDB plugin for xnvme
fs, the user-space file system at the top of this repository. The code here
is the part of the repository that links DuckDB or RocksDB.

With an adapter, an engine stores its files in a pool. `xnvme fs serve`
serves the metadata of the pool on a Unix socket. The threads of the engine
read and write the members of the pool directly, each thread on its own NVMe
queues.

| Path | Content |
|---|---|
| `include/yace/xnvme_fs.hpp` | The C++ layer over the C interface `libxnvme_fs.h`. yace generates it for C++17, and the generated file is committed. It has classes for a session, a file and a request that free their C handles in their destructors. A failed call throws a `std::system_error` with the errno and the message of the library. `scripts/generate-cpp.sh` regenerates the file and names the yace version that it needs. |
| `include/xnvme_fs.hpp` | The classes that the adapters add on top of the generated layer: a session that records whether its connection to the server still works, and files that record which handles refer to the same file of the pool. |
| `src/` | The DuckDB extension `xnvme_fs`: the file system for paths `xnvme-fs://POOL/NAME`, its settings, the table function `xnvme_fs_pool_info()`, and write-behind for temporary files. |
| `rocksdb/` | The RocksDB `FileSystem` plugin for `--fs_uri=xnvme-fs://POOL`. |
| `bench/` | `xnvme-fs-bench`, which runs a workload on one file of a pool. |
| `test/` | The tests of the adapters. `test/with-xnvme-fs.sh` runs each test beside `xnvme fs serve`. |

## Building

The build finds libxnvme-fs by pkg-config. The meson project at the top of
this repository installs libxnvme-fs beside xNVMe, and the top `README.md`
describes that build. When xNVMe and libxnvme-fs are installed in a prefix
other than the system's, set `PKG_CONFIG_PATH` to the pkg-config directory of
that prefix:

    export PKG_CONFIG_PATH=$PREFIX/lib/x86_64-linux-gnu/pkgconfig
    GEN=ninja make release      # DuckDB with the extension, behind_test, xnvme-fs-bench
    make rocksdb                # db_bench and RocksDB's tests with the plugin, in a Debug build

## Using

    xnvme homi serve 0000:01:00.0 --be upcie --homi-id 1 &
    export XNVME_FS_HOMI_ID=1
    xnvme fs mkfs --pool pool0 --member 0000:01:00.0 --homi-id 1
    xnvme fs serve --pool pool0 --member 0000:01:00.0 --homi-id 1 &
    duckdb -c "LOAD xnvme_fs; ATTACH 'xnvme-fs://pool0/tpch.db' AS t;"
    db_bench --fs_uri=xnvme-fs://pool0 --db=/bench

`../docs/adapters/duckdb.md` and `../docs/adapters/rocksdb.md` describe the
adapters and their settings. `../docs/tools.md` describes the environment
variables, and `../docs/design.md` describes the design.

## Testing

The tests need `xnvme` in `PATH`, a HOMI server that serves the controllers,
and the controllers named in `XNVME_FS_MEMBER`. The tests overwrite those
controllers.

    export XNVME_FS_HOMI_ID=1 XNVME_FS_MEMBER="0000:01:00.0 0000:02:00.0"
    make test_xnvme_fs                              # the extension's sqllogictests, and behind_test
    make test_xnvme_fs_duckdb_suite                 # DuckDB's storage tests on a pool
    test/with-xnvme-fs.sh test/rocksdb.sh
    test/with-xnvme-fs.sh test/two-engines.sh
    test/two-pools.sh                               # a pool on each of the first two controllers, one DuckDB
    test/with-xnvme-fs.sh test/bench.sh

## License

The code is licensed MIT, as DuckDB and its extensions are. The documentation
is licensed CC-BY-4.0. The directory is REUSE compliant.
