#!/bin/sh
# SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
#
# SPDX-License-Identifier: MIT
#
# DuckDB and RocksDB on the pool "ci" that 'xnvme fs serve' serves, at once, as with-xnvme-fs.sh starts it: db_bench fills
# a database under /rocksdb while DuckDB loads and checkpoints a table under /duckdb, round after round, until
# db_bench is done. Each engine then reopens its own database and checks it: RocksDB every checksum, DuckDB the
# rows of every round. Built by make and make rocksdb.
set -eu

DUCKDB=${DUCKDB:-build/release/duckdb}
ROCKSDB=${ROCKSDB:-build/rocksdb}
work=$(mktemp -d "${TMPDIR:-/tmp}/xnvme-fs-two.XXXXXX")
trap 'rm -rf "$work"' EXIT

duck() {
	"$DUCKDB" -noheader -list -c "LOAD xnvme_fs; ATTACH 'xnvme-fs://ci/duckdb/two.db' AS d; $1"
}

"$ROCKSDB/db_bench" --fs_uri=xnvme-fs://ci --db=/rocksdb/two --compression_type=none --num=20000 --threads=2 \
	--benchmarks=fillrandom --verify_checksum=true >"$work/db_bench.log" 2>&1 &
bench=$!

duck "CREATE TABLE d.t (round INTEGER, i BIGINT);" >/dev/null
rounds=0
while kill -0 "$bench" 2>/dev/null || [ "$rounds" -lt 2 ]; do
	duck "INSERT INTO d.t SELECT $rounds, range FROM range(100000); CHECKPOINT d;" >/dev/null
	rounds=$((rounds + 1))
done
rc=0
wait "$bench" || rc=$?
cat "$work/db_bench.log"
if [ "$rc" -ne 0 ]; then
	echo "two-engines.sh: db_bench failed while DuckDB ran ($rounds rounds)" >&2
	exit 1
fi

# Two threads of random keys cover about 1 - e^-2, 86%, of the key space; far fewer found means lost writes
found=$("$ROCKSDB/db_bench" --fs_uri=xnvme-fs://ci --db=/rocksdb/two --compression_type=none --num=20000 \
	--use_existing_db=1 --benchmarks=verifychecksum,readrandom | sed -n 's/.*(\([0-9]*\) of 20000 found).*/\1/p')
if [ -z "$found" ] || [ "$found" -lt 16000 ]; then
	echo "two-engines.sh: the reopened RocksDB found '$found' of 20000 keys" >&2
	exit 1
fi

got=$(duck "SELECT count(*), count(DISTINCT round), sum(i) FROM d.t;")
want="$((rounds * 100000))|$rounds|$((rounds * 4999950000))"
if [ "$got" != "$want" ]; then
	echo "two-engines.sh: DuckDB holds '$got', not '$want'" >&2
	exit 1
fi

# One namespace: DuckDB sees RocksDB's CURRENT in the pool beside its own database
seen=$(duck "SELECT count(*) FROM glob('xnvme-fs://ci/rocksdb/two/CURRENT');")
if [ "$seen" != "1" ]; then
	echo "two-engines.sh: DuckDB does not see RocksDB's CURRENT in the pool" >&2
	exit 1
fi
echo "two-engines.sh: ok, $rounds DuckDB rounds beside db_bench, $found of 20000 keys found"
