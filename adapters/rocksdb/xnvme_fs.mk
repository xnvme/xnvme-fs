# SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
#
# SPDX-License-Identifier: MIT
#
# RocksDB's CMake build reads a plugin's registration function from here, and its sources, paths and libraries
# from CMakeLists.txt beside it; listing the sources here as well would build them twice.
xnvme_fs_FUNC = RegisterXnvmeFs
