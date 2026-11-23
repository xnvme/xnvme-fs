# SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
#
# SPDX-License-Identifier: MIT
#
# The xnvme fs plugin for RocksDB's CMake build, which adds this directory when it is linked in as plugin/xnvme_fs.
# Paths resolve through that link to the adapters' tree. libxnvme-fs comes from an installed xnvme fs, by pkg-config.
get_filename_component(XNVME_FS_PLUGIN_DIR "${CMAKE_CURRENT_LIST_DIR}" REALPATH)
get_filename_component(XNVME_FS_ADAPTERS_DIR "${XNVME_FS_PLUGIN_DIR}/.." ABSOLUTE)

find_package(PkgConfig REQUIRED)
pkg_check_modules(XNVME_FS REQUIRED xnvme-fs)

set(xnvme_fs_SOURCES xnvme_fs_rocksdb.cc PARENT_SCOPE)
set(xnvme_fs_INCLUDE_PATHS "${XNVME_FS_ADAPTERS_DIR}/include" ${XNVME_FS_INCLUDE_DIRS} PARENT_SCOPE)
# The libraries by their full paths, so the build's binaries find a prefix that is not the system's
set(xnvme_fs_LIBS ${XNVME_FS_LINK_LIBRARIES} PARENT_SCOPE)

# Built only when named, by make rocksdb: the static library carries the plugin
add_executable(xnvme_fs_async_read_test EXCLUDE_FROM_ALL "${XNVME_FS_PLUGIN_DIR}/async_read_test.cc")
target_link_libraries(xnvme_fs_async_read_test rocksdb)
set_target_properties(xnvme_fs_async_read_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
