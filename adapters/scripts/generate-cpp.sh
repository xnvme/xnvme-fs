#!/bin/sh
# SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
#
# SPDX-License-Identifier: MIT
#
# Generates adapters/include/yace/xnvme_fs.hpp, the C++ layer over include/libxnvme_fs.h that the adapters use, with
# yace's cpp target. The layer is generated for C++17, which the DuckDB extension is built as, and compiles as
# C++20, which RocksDB builds its plugin as.
#
#   generate-cpp.sh           writes the header
#   generate-cpp.sh --check   fails where the header differs from what yace generates now; exits 77, which meson
#                             reports as a skipped test, where no yace that can generate the layer is installed
#
# No release of yace has the option cpp.std or the meta key errmsg. The committed header was generated with yace at
# the commit below, of its branch cpp-std-errmsg; install that, or a later one, for 'yace' to be on PATH:
#
#   f62d666 fix(cpp): say what the signature has for a parameter the doc names
set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
header="$root/adapters/include/yace/xnvme_fs.hpp"
check=0
if [ "${1:-}" = "--check" ]; then
	check=1
elif [ $# -ne 0 ]; then
	echo "usage: generate-cpp.sh [--check]" >&2
	exit 64
fi

# A yace without --target-option is a release older than the branch, which cannot generate the layer
if ! command -v yace > /dev/null 2>&1 || ! yace --help | grep -q -e --target-option; then
	echo "generate-cpp.sh: no yace with --target-option is installed" >&2
	[ $check -eq 1 ] && exit 77
	exit 1
fi

out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

# Comparing needs no compiler: --no-check leaves out the build and run of the layer's check program
if ! yace --emit cpp --target-option cpp.std=c++17 --meta "$root/adapters/include/yace/xnvme_fs.meta.yaml" \
	--output "$out" -I "$root/include" $([ $check -eq 1 ] && echo --no-check) \
	"$root/include/libxnvme_fs.h" > "$out/yace.log" 2>&1; then
	cat "$out/yace.log" >&2
	exit 1
fi

if [ $check -eq 1 ]; then
	if ! diff -u "$header" "$out/cpp/xnvme_fs.hpp"; then
		echo "generate-cpp.sh: $header is not what yace generates from include/libxnvme_fs.h; run generate-cpp.sh" >&2
		exit 1
	fi
else
	cp "$out/cpp/xnvme_fs.hpp" "$header"
fi
