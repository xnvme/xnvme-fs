# SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
# SPDX-FileCopyrightText: Stichting DuckDB Foundation
#
# SPDX-License-Identifier: MIT

PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=xnvme_fs
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# The adapters' tests on the controllers in XNVME_FS_MEMBER, held by the HOMI server XNVME_FS_HOMI_ID, each beside
# 'xnvme fs serve' on a fresh pool: the extension's sqllogictests, the slow ones among them, and the writes behind.
# 'xnvme' comes from PATH, from the prefix the build found libxnvme-fs in.
.PHONY: test_xnvme_fs test_xnvme_fs_duckdb_suite
test_xnvme_fs:
	test/with-xnvme-fs.sh ./build/release/$(TEST_PATH) "test/*"
	test/with-xnvme-fs.sh ./build/release/extension/xnvme_fs/behind_test ci

# RocksDB with the xnvme fs plugin linked in as plugin/xnvme_fs: db_bench, and the conformance and basic tests that
# take a file system by TEST_FS_URI. A Debug build, since RocksDB builds its tests in no other and keeps its
# assertions on in it; the small set of tests is exactly those two. For correctness, not for measuring.
ROCKSDB_BUILD := build/rocksdb
.PHONY: rocksdb
rocksdb:
	ln -sfn ../../../rocksdb third_party/rocksdb/plugin/xnvme_fs
	cmake -S third_party/rocksdb -B $(ROCKSDB_BUILD) -G Ninja -DCMAKE_BUILD_TYPE=Debug -DPORTABLE=1 \
		-DFAIL_ON_WARNINGS=OFF -DWITH_GFLAGS=ON -DWITH_ALL_TESTS=OFF -DWITH_BENCHMARK_TOOLS=ON \
		-DROCKSDB_BUILD_SHARED=OFF -DROCKSDB_PLUGINS=xnvme_fs
	ninja -C $(ROCKSDB_BUILD) db_bench env_basic_test db_basic_test xnvme_fs_async_read_test

# db_bench alone in a Release build, for measuring: the plugin as above, without tests or RocksDB's assertions.
ROCKSDB_RELEASE := build/rocksdb-release
.PHONY: rocksdb-release
rocksdb-release:
	ln -sfn ../../../rocksdb third_party/rocksdb/plugin/xnvme_fs
	cmake -S third_party/rocksdb -B $(ROCKSDB_RELEASE) -G Ninja -DCMAKE_BUILD_TYPE=Release -DPORTABLE=1 \
		-DFAIL_ON_WARNINGS=OFF -DWITH_GFLAGS=ON -DWITH_TESTS=OFF -DWITH_BENCHMARK_TOOLS=ON \
		-DROCKSDB_BUILD_SHARED=OFF -DROCKSDB_PLUGINS=xnvme_fs
	ninja -C $(ROCKSDB_RELEASE) db_bench

# db_bench from RocksDB's source as its tag has it, with the Release build's options and no plugin: stock RocksDB,
# what the runs on XFS measure.
ROCKSDB_STOCK := build/rocksdb-stock
.PHONY: rocksdb-stock
rocksdb-stock:
	cmake -S third_party/rocksdb -B $(ROCKSDB_STOCK) -G Ninja -DCMAKE_BUILD_TYPE=Release -DPORTABLE=1 \
		-DFAIL_ON_WARNINGS=OFF -DWITH_GFLAGS=ON -DWITH_TESTS=OFF -DWITH_BENCHMARK_TOOLS=ON \
		-DROCKSDB_BUILD_SHARED=OFF
	ninja -C $(ROCKSDB_STOCK) db_bench

# DuckDB's own storage tests with each test's database and temporary files on the pool, a pool to each directory.
test_xnvme_fs_duckdb_suite:
	test/duckdb-suite.sh $(PROJ_DIR)test/configs/xnvme_fs.json
