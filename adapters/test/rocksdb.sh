#!/bin/sh
# SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
#
# SPDX-License-Identifier: MIT
#
# RocksDB on the pool "ci" that 'xnvme fs serve' serves, as with-xnvme-fs.sh starts it, through the xnvme fs plugin:
# RocksDB's file system conformance tests, its basic database tests, and db_bench filling a database and reading it
# back, MultiGet through the asynchronous path among them; and a read started asynchronously from a stack frame that
# is gone before it is polled. Built by make rocksdb.
set -eu

ROCKSDB=${ROCKSDB:-build/rocksdb}
export TEST_FS_URI=xnvme-fs://ci

"$ROCKSDB/env_basic_test" --gtest_filter='CustomEnv/*'
"$ROCKSDB/xnvme_fs_async_read_test"
# A crashing test is run again alone under gdb, for its backtrace; the step fails either way
log=$(mktemp)
trap 'rm -f "$log"' EXIT
if ! "$ROCKSDB/db_basic_test" >"$log" 2>&1; then
	cat "$log"
	crashed=$(sed -n 's/^\[ RUN      \] //p' "$log" | tail -n 1)
	if [ -n "$crashed" ] && ! grep -q "^\[       OK \] $crashed\|^\[  FAILED  \] $crashed" "$log"; then
		gdb -q -batch -ex run -ex "thread apply all bt" --args "$ROCKSDB/db_basic_test" --gtest_filter="$crashed"
	fi
	exit 1
fi
cat "$log"

"$ROCKSDB/db_bench" --fs_uri=xnvme-fs://ci --db=/dbbench --compression_type=none --num=200000 --threads=2 \
	--benchmarks=fillrandom,compact,readrandom,multireadrandom,readseq --async_io=true --verify_checksum=true
"$ROCKSDB/db_bench" --fs_uri=xnvme-fs://ci --db=/dbbench --compression_type=none --num=200000 \
	--use_existing_db=1 --benchmarks=verifychecksum,readrandom
# Writes only in the write-ahead log, never synced, read back by the next process: the log outlives its writer
"$ROCKSDB/db_bench" --fs_uri=xnvme-fs://ci --db=/walonly --compression_type=none --num=5000 --benchmarks=fillseq \
	--sync=false
found=$("$ROCKSDB/db_bench" --fs_uri=xnvme-fs://ci --db=/walonly --compression_type=none --num=5000 \
	--use_existing_db=1 --benchmarks=readrandom | sed -n 's/.*(\([0-9]*\) of \([0-9]*\) found).*/\1 of \2/p')
if [ "$found" != "5000 of 5000" ]; then
	echo "rocksdb.sh: the reopened database found '$found' of its 5000 logged keys" >&2
	exit 1
fi
echo "rocksdb.sh: ok"
