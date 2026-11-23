# SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
# SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
#
# SPDX-License-Identifier: BSD-3-Clause

fs_bin = executable(
  'xnvme-fs',
  files('main.cpp', 'server.cpp', 'view.cpp', 'cp.c'),
  include_directories: [fs_inc, fs_core_inc],
  link_args: link_args_hardening,
  pie: get_option('hardening'),
  link_with: [fs_core, fs_lib],
  dependencies: [xnvme_dep, thread_dep, ufuse_dep],
  install_rpath: fs_rpath,
  install: true,
  install_dir: xnvme_libexecdir,
)

# In xNVMe's extension directory, so that the overview of 'xnvme' lists 'xnvme fs' in its serve section; copied into
# the build directory too, which cli_check.py gives 'xnvme' as its exec path and its extension directory
configure_file(
  input: 'fs.extension',
  output: 'fs.extension',
  copy: true,
  install: true,
  install_dir: xnvme_extensiondir,
)

# xnvme-fs under the names mkfs(8) and mount(8) look their helpers up by, 'mkfs -t xnvme-fs' and
# 'mount -t xnvme-fs': it takes their command lines when run as one of the two
fs_helpers = []
foreach helper : ['mkfs.xnvme-fs', 'mount.xnvme-fs']
  install_symlink(
    helper,
    pointing_to: xnvme_libexecdir / 'xnvme-fs',
    install_dir: get_option('sbindir'),
  )
  fs_helpers += custom_target(
    helper,
    output: helper,
    command: ['ln', '-sf', 'xnvme-fs', '@OUTPUT@'],
    depends: fs_bin,
    build_by_default: true,
  )
endforeach

# The front-end of the xNVMe this builds against, where that is installed
xnvme_bin = find_program('xnvme', dirs: [xnvme_prefix / 'bin'], required: false)

test(
  'cli, the extension, the helpers and their usage',
  python3,
  args: [files('cli_check.py'), fs_bin, fs_helpers] + (xnvme_bin.found() ? [xnvme_bin.full_path()] : []),
  suite: 'cli',
)
