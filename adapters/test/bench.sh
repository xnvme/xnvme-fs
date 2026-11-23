#!/bin/sh
# SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
#
# SPDX-License-Identifier: MIT
#
# xnvme-fs-bench on the pool "ci" that 'xnvme fs serve' serves, as with-xnvme-fs.sh starts it: every workload through the
# synchronous and the asynchronous interface, briefly, with every sector checked, polling paced and spinning
# (XNVME_FS_POLL_MODE). A correctness run, not a measurement; emulated controllers make numbers that mean nothing.
# One JSON line per run.
set -eu

BENCH=${BENCH:-build/release/extension/xnvme_fs/xnvme-fs-bench}
for poll in paced spin; do
	for workload in randread seqread randwrite seqwrite walsync; do
		for api in sync async; do
			XNVME_FS_POLL_MODE=$poll "$BENCH" --pool ci --file "bench-$workload-$api" --size 256M --workload "$workload" \
				--api "$api" --qd 16 --threads 2 --seconds 3 --verify
		done
	done
done
echo "bench.sh: ok"
