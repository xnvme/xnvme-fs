#!/bin/sh
# SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
#
# SPDX-License-Identifier: MIT
#
# DuckDB's storage tests with each test's database and temporary files on a pool, a fresh pool for each directory
# of test/sql/storage and one for the files at its top: a test leaves its database in the pool, and DuckDB 2.0 has
# more of them than one pool of the guest's members holds. After each group, the pool's use is printed. Fails if any
# group fails. Takes what with-xnvme-fs.sh takes, and the test config as its argument.
#
#   XNVME_FS_HOMI_ID=1 XNVME_FS_MEMBER="0000:01:00.0 0000:02:00.0" test/duckdb-suite.sh test/configs/xnvme_fs.json
set -eu

config=$1
unittest=./build/release/test/unittest
rc=0
group() {
	echo "== $1"
	test/with-xnvme-fs.sh sh -c "rc=0; $unittest --test-dir duckdb --test-config $config '$2' || rc=\$?;
		./build/release/duckdb -c '.read test/pool-usage.sql'; exit \$rc" || rc=1
}
for d in duckdb/test/sql/storage/*/; do
	d=${d#duckdb/}
	group "$d" "$d*"
done
top=$(cd duckdb && ls test/sql/storage/*.test test/sql/storage/*.test_slow 2>/dev/null | paste -sd, -)
group "test/sql/storage/ (top)" "$top"
exit "$rc"
