# SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
# SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
#
# SPDX-License-Identifier: BSD-3-Clause

fs_core_inc = include_directories('.')
# The C++ classes, linked into libxnvme-fs and into the server and tests, which use them
# directly; libxnvme-fs exports only its C interface, by libxnvme-fs.map. It is a shared library
# only: an archive has no version script, so it would define the classes in whatever links it,
# and a program in C would need the C++ runtime to link it
fs_core = static_library(
  'xnvme-fs-core',
  files('layout.cpp', 'protocol.cpp', 'session.cpp', 'trace.cpp'),
  include_directories: fs_inc,
  pic: true,
  dependencies: [xnvme_dep, thread_dep],
  install: false,
)
fs_lib = shared_library(
  'xnvme-fs',
  files('capi.cpp'),
  include_directories: fs_inc,
  link_whole: fs_core,
  link_args: ['-Wl,--version-script=@0@'.format(meson.current_source_dir() / 'libxnvme-fs.map')],
  link_depends: files('libxnvme-fs.map'),
  dependencies: [xnvme_dep, thread_dep],
  version: meson.project_version(),
  install_rpath: fs_rpath,
  install: true,
)

pconf.generate(
  fs_lib,
  version: meson.project_version(),
  requires: ['xnvme'],
  name: 'xNVMe fs',
  filebase: 'xnvme-fs',
  description: 'xnvme fs: a user-space file system on NVMe drives, built on xNVMe',
)
