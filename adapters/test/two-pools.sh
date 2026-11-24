#!/bin/sh
# SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
#
# SPDX-License-Identifier: MIT
#
# One DuckDB process with a database on each of two pools: "ci1" on the first controller in XNVME_FS_MEMBER and "ci2"
# on the second, each served by its own 'xnvme fs serve' with its socket in one directory. The process writes and
# checkpoints a table in each database and reads both back; a second process then reopens both and checks that each
# pool holds its own database and not the other's. Takes what with-xnvme-fs.sh takes, with at least two members.
#
#   XNVME_FS_HOMI_ID=1 XNVME_FS_MEMBER="0000:01:00.0 0000:02:00.0" test/two-pools.sh
set -eu

XNVME=${XNVME:-xnvme}
DUCKDB=${DUCKDB:-build/release/duckdb}
work=$(mktemp -d "${TMPDIR:-/tmp}/xnvme-fs-two-pools.XXXXXX")
pids=""
cleanup() {
	for p in $pids; do kill "$p" 2>/dev/null || true; done
	rm -rf "$work"
}
trap cleanup EXIT

backend=${XNVME_FS_BACKEND:-upcie}
set -- ${XNVME_FS_MEMBER:-}
if [ "$#" -lt 2 ] || { [ "$backend" = upcie ] && [ -z "${XNVME_FS_HOMI_ID:-}" ]; }; then
	echo "$(basename "$0"): needs two members in XNVME_FS_MEMBER, and XNVME_FS_HOMI_ID for controllers held by HOMI" >&2
	exit 64
fi
member1=$1
member2=$2
access="--backend $backend"
if [ -n "${XNVME_FS_HOMI_ID:-}" ]; then
	access="$access --homi-id $XNVME_FS_HOMI_ID"
fi

serve() {
	"$XNVME" fs mkfs --pool "$1" --member "$2" $access --extent-size 1M --overwrite >/dev/null
	# The front end execs the server, so the pid is the server's
	"$XNVME" fs serve --pool "$1" --member "$2" $access --socket "$work/$1.sock" 2>"$work/$1.log" &
	pids="$pids $!"
	tries=0
	while [ ! -S "$work/$1.sock" ]; do
		tries=$((tries + 1))
		if [ "$tries" -gt 100 ] || ! kill -0 "$!" 2>/dev/null; then
			cat "$work/$1.log" >&2
			exit 1
		fi
		sleep 0.1
	done
}
serve ci1 "$member1"
serve ci2 "$member2"

duck() {
	XNVME_FS_SOCKET_DIR=$work "$DUCKDB" -noheader -list -c "LOAD xnvme_fs; $1"
}
attach="ATTACH 'xnvme-fs://ci1/one.db' AS a; ATTACH 'xnvme-fs://ci2/two.db' AS b;"
want="ci1|100000|4999950000
ci2|50000|2499950000"

got=$(duck "$attach
	CREATE TABLE a.t AS SELECT 'ci1' AS pool, range AS i FROM range(100000);
	CREATE TABLE b.t AS SELECT 'ci2' AS pool, range * 2 AS i FROM range(50000);
	CHECKPOINT a; CHECKPOINT b;
	SELECT pool, count(*), sum(i) FROM a.t GROUP BY pool UNION ALL
	SELECT pool, count(*), sum(i) FROM b.t GROUP BY pool ORDER BY pool;" | tail -n 2)
if [ "$got" != "$want" ]; then
	echo "two-pools.sh: the writing process read '$got', not '$want'" >&2
	exit 1
fi

got=$(duck "$attach
	SELECT pool, count(*), sum(i) FROM a.t GROUP BY pool UNION ALL
	SELECT pool, count(*), sum(i) FROM b.t GROUP BY pool ORDER BY pool;")
if [ "$got" != "$want" ]; then
	echo "two-pools.sh: the reopened databases hold '$got', not '$want'" >&2
	exit 1
fi

# Each pool holds its own database and not the other's, and each pool's server reports its own member
got=$(duck "SELECT
	(SELECT count(*) FROM glob('xnvme-fs://ci1/one.db')), (SELECT count(*) FROM glob('xnvme-fs://ci1/two.db')),
	(SELECT count(*) FROM glob('xnvme-fs://ci2/two.db')), (SELECT count(*) FROM glob('xnvme-fs://ci2/one.db'));
	SELECT pool, member FROM xnvme_fs_pool_info('ci1') UNION ALL
	SELECT pool, member FROM xnvme_fs_pool_info('ci2') ORDER BY pool;")
want="1|0|1|0
ci1|$member1
ci2|$member2"
if [ "$got" != "$want" ]; then
	echo "two-pools.sh: the pools hold '$got', not '$want'" >&2
	exit 1
fi

rc=0
for p in $pids; do
	if ! kill "$p" 2>/dev/null; then
		echo "two-pools.sh: an 'xnvme fs serve' died during the run" >&2
		rc=1
	fi
	wait "$p" || true
done
pids=""
if [ "$rc" -ne 0 ]; then
	cat "$work/ci1.log" "$work/ci2.log" >&2
	exit 1
fi
echo "two-pools.sh: ok, one DuckDB process on ci1 ($member1) and ci2 ($member2)"
