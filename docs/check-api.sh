#!/bin/sh
# SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
#
# SPDX-License-Identifier: MIT
#
# Fails where api.md does not name a function, struct or macro that libxnvme_fs.h declares, so the hand-written
# reference cannot fall behind the header. The Makefile runs it before Sphinx.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
symbols=$(sed -nE \
	-e 's/^(xnvme_fs_[a-z_]+)\(.*/\1/p' \
	-e 's/^struct (xnvme_fs_[a-z_]+)( \{|;).*/\1/p' \
	-e 's/^#define (XNVME_FS_[A-Z_]+)[[:space:]].*/\1/p' \
	"$here/../include/libxnvme_fs.h")
if [ -z "$symbols" ]; then
	echo "check-api.sh: found no declarations in libxnvme_fs.h" >&2
	exit 1
fi
missing=0
for s in $symbols; do
	if ! grep -qw "$s" "$here/api.md"; then
		echo "check-api.sh: docs/api.md does not name $s" >&2
		missing=1
	fi
done
exit $missing
