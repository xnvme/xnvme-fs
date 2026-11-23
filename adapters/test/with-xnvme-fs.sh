#!/bin/sh
# SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
#
# SPDX-License-Identifier: MIT
#
# Runs a command beside 'xnvme fs serve' on a fresh pool "ci", with XNVME_FS_SOCKET_DIR pointing at its socket, and
# fails if the command fails or the server did not survive it. The pool is the controllers in XNVME_FS_MEMBER,
# space-separated, which are overwritten, held by the HOMI server XNVME_FS_HOMI_ID; or with XNVME_FS_BACKEND set to
# one of the kernel's, the namespaces' block or character devices, and no HOMI. XNVME names the xnvme program, else
# the one in PATH.
#
#   XNVME_FS_HOMI_ID=1 XNVME_FS_MEMBER="0000:01:00.0 0000:02:00.0" test/with-xnvme-fs.sh ./build/release/test/unittest "test/*"
set -eu

XNVME=${XNVME:-xnvme}
work=$(mktemp -d "${TMPDIR:-/tmp}/xnvme-fs-ci.XXXXXX")
trap 'rm -rf "$work"' EXIT

backend=${XNVME_FS_BACKEND:-upcie}
if [ -z "${XNVME_FS_MEMBER:-}" ] || { [ "$backend" = upcie ] && [ -z "${XNVME_FS_HOMI_ID:-}" ]; }; then
	echo "$(basename "$0"): needs XNVME_FS_MEMBER, and XNVME_FS_HOMI_ID for controllers held by HOMI" >&2
	exit 64
fi
access="--backend $backend"
if [ -n "${XNVME_FS_HOMI_ID:-}" ]; then
	access="$access --homi-id $XNVME_FS_HOMI_ID"
fi
members=""
for m in $XNVME_FS_MEMBER; do members="$members --member $m"; done
"$XNVME" fs mkfs --pool ci $members $access --extent-size 1M --overwrite >/dev/null
# The front end execs the server, so the pid is the server's
"$XNVME" fs serve --pool ci $members $access --socket "$work/ci.sock" 2>"$work/serve.log" &
pid=$!
tries=0
while [ ! -S "$work/ci.sock" ]; do
	tries=$((tries + 1))
	if [ "$tries" -gt 100 ] || ! kill -0 "$pid" 2>/dev/null; then
		cat "$work/serve.log" >&2
		kill "$pid" 2>/dev/null || true
		exit 1
	fi
	sleep 0.1
done

rc=0
XNVME_FS_SOCKET_DIR=$work "$@" || rc=$?

if ! kill "$pid" 2>/dev/null; then
	echo "with-xnvme-fs: 'xnvme fs serve' died during the run" >&2
	rc=1
fi
wait "$pid" || true
if [ "$rc" -ne 0 ]; then
	cat "$work/serve.log" >&2
fi
exit "$rc"
