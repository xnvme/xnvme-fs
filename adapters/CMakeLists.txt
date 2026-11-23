# SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
# SPDX-FileCopyrightText: Stichting DuckDB Foundation
#
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME xnvme_fs)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# libxnvme-fs as the meson project at the top of this repository installs it; PKG_CONFIG_PATH names its prefix where
# that is not the system's. The same prefix has 'xnvme fs', which serves the pools.
find_package(PkgConfig REQUIRED)
pkg_check_modules(XNVME_FS REQUIRED IMPORTED_TARGET xnvme-fs)

include_directories(src/include include)

# The extension, and what links DuckDB, where DuckDB's build includes this file; run on its own, cmake builds only
# what needs no DuckDB
if(COMMAND build_static_extension)
  set(EXTENSION_SOURCES
      src/xnvme_fs_extension.cpp src/xnvme_fs_filesystem.cpp src/pool_connection.cpp
      src/pool_functions.cpp src/write_behind.cpp)

  build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
  build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

  target_link_libraries(${EXTENSION_NAME} PkgConfig::XNVME_FS)
  target_link_libraries(${LOADABLE_EXTENSION_NAME} PkgConfig::XNVME_FS)

  # The adapter's writes behind need DuckDB's library, which the extension's static library is built against
  add_executable(behind_test test/behind_test.cpp)
  target_link_libraries(behind_test ${EXTENSION_NAME} duckdb_static)
  link_extension_libraries(behind_test "")

  install(
    TARGETS ${EXTENSION_NAME}
    EXPORT "${DUCKDB_EXPORT_SET}"
    LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
    ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
endif()

add_executable(xnvme-fs-bench bench/xnvme_fs_bench.cpp)
target_link_libraries(xnvme-fs-bench PkgConfig::XNVME_FS pthread)
