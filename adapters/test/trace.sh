#!/bin/sh
# SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
#
# SPDX-License-Identifier: MIT
#
# What each engine asks of the pool "ci" that 'xnvme fs serve' serves, as with-xnvme-fs.sh starts it, traced by the library
# through XNVME_FS_TRACE: DuckDB generating TPC-H SF1, then in a second process running its 22 queries and in a
# third deleting and adding rows, and RocksDB's db_bench filling,
# compacting and reading. For each engine it prints every operation used with its count and bytes, and a histogram
# of read and write sizes; the traces stay in TRACE_DIR when that is set. Fails if an engine used none. THREADS, when
# set, caps DuckDB's threads and the threads RocksDB opens its files with, as a drive with few queues needs.
set -eu

DUCKDB=${DUCKDB:-build/release/duckdb}
ROCKSDB=${ROCKSDB:-build/rocksdb}
dir=${TRACE_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/xnvme-fs-trace.XXXXXX")}
mkdir -p "$dir"
rm -f "$dir/duckdb.trace" "$dir/rocksdb.trace"
threads=${THREADS:+SET threads=$THREADS;}

# Generated in one process and queried in another, so the queries read the database from the pool rather than
# finding it in the generating process's buffer pool
XNVME_FS_TRACE="$dir/duckdb.trace" "$DUCKDB" -c "LOAD xnvme_fs; LOAD tpch; $threads
ATTACH 'xnvme-fs://ci/trace/tpch.db' AS t; USE t; CALL dbgen(sf=1); CHECKPOINT;" >/dev/null
queries=""
for q in $(seq 1 22); do queries="$queries PRAGMA tpch($q);"; done
XNVME_FS_TRACE="$dir/duckdb.trace" "$DUCKDB" -c "LOAD xnvme_fs; LOAD tpch; $threads
ATTACH 'xnvme-fs://ci/trace/tpch.db' AS t; USE t; $queries" >/dev/null
# Rows deleted and added as TPC-H's refresh functions do, so a checkpoint writes into blocks it freed
XNVME_FS_TRACE="$dir/duckdb.trace" "$DUCKDB" -c "LOAD xnvme_fs; $threads
ATTACH 'xnvme-fs://ci/trace/tpch.db' AS t; USE t;
DELETE FROM lineitem WHERE l_orderkey % 100 = 0; DELETE FROM orders WHERE o_orderkey % 100 = 0; CHECKPOINT;
INSERT INTO orders SELECT * FROM orders WHERE o_orderkey % 100 = 1;
INSERT INTO lineitem SELECT * FROM lineitem WHERE l_orderkey % 100 = 1; CHECKPOINT;" >/dev/null

XNVME_FS_TRACE="$dir/rocksdb.trace" "$ROCKSDB/db_bench" --fs_uri=xnvme-fs://ci --db=/trace/rocksdb \
	--compression_type=none --num=100000 --benchmarks=fillrandom,compact,readrandom,multireadrandom \
	--async_io=true ${THREADS:+--file_opening_threads=$THREADS} >/dev/null

# Operations by count and bytes, then read and write sizes in power-of-two buckets
summarise() {
	awk -v engine="$1" '
	{ op = $4; count[op]++
	  if (op ~ /read|write/) bytes[op] += $7
	  if (op == "lease") extents += $7
	  if (op ~ /read|write/ && $7 > 0) { b = 1; while (b < $7) b *= 2; size[op " " b]++ } }
	END {
		n = 0; for (op in count) n++
		printf "%s: %d operations used, %d extents leased\n", engine, n, extents
		printf "  %-14s %12s %16s\n", "operation", "count", "bytes"
		for (op in count) printf "  %-14s %12d %16d\n", op, count[op], bytes[op] | "sort"
		close("sort")
		printf "  %-14s %12s %12s\n", "sizes", "up to", "count"
		for (k in size) { split(k, p, " "); printf "  %-14s %12d %12d\n", p[1], p[2], size[k] | "sort -k1,1 -k2,2n" }
		close("sort -k1,1 -k2,2n")
	}' "$2"
}

for engine in duckdb rocksdb; do
	if [ ! -s "$dir/$engine.trace" ]; then
		echo "trace.sh: $engine left no trace" >&2
		exit 1
	fi
	summarise "$engine" "$dir/$engine.trace"
done
echo "trace.sh: ok, traces in $dir"
