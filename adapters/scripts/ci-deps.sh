#!/bin/sh
# SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
#
# SPDX-License-Identifier: MIT
#
# The packages the adapters need to build and test, on Debian and Ubuntu, beside an installed xNVMe and xnvme fs.
set -eu
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
    build-essential ca-certificates ccache cmake git libaio-dev liburing-dev libgflags-dev \
    libnuma-dev meson ninja-build pkg-config python3 uuid-dev
