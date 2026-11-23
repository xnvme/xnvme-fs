# SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
# SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
#
# SPDX-License-Identifier: BSD-3-Clause

# Each needs controllers held by a HOMI server, so cijoe/tests/test_fs.py runs them rather than meson, as
# 'xnvme test fs-<name>'
if get_option('tests')
  foreach name, source : {
    'member': 'member.cpp',
    'poke': 'poke.cpp',
    'index': 'index.cpp',
    'capi': 'capi.c',
    'hold': 'hold.cpp',
    'crash': 'crash.cpp',
  }
    executable(
      'xnvme-test-fs-' + name,
      source,
      include_directories: [fs_inc, fs_core_inc],
      # capi takes the C interface as a program outside this project does; the others take the classes
      link_with: name == 'capi' ? fs_lib : fs_core,
      link_args: link_args_hardening,
      pie: get_option('hardening'),
      dependencies: [xnvme_dep, thread_dep],
      install_rpath: fs_rpath,
      install: true,
      install_dir: xnvme_libexecdir,
    )
  endforeach
endif

test(
  'fs, exports',
  python3,
  args: [files('exports.py'), find_program('nm').full_path(), fs_lib],
)

# The adapters, in adapters/, are left behind where only the file system is copied to a host to be built, as
# cijoe/scripts/xnvme_fs_build.py does; their C++ layer is tested where they are
if import('fs').is_dir(meson.project_source_root() / 'adapters' / 'include')
  # The adapters' header on top of the layer yace generates from libxnvme_fs.h, built as both standards the adapters
  # are built as, and run as far as it goes without a server
  adapters_inc = include_directories('../adapters/include')
  foreach std : ['c++17', 'c++20']
    test(
      'cpp, the generated layer as ' + std,
      executable(
        'xnvme-test-fs-layer-' + std,
        'layer.cpp',
        include_directories: [fs_inc, adapters_inc],
        link_with: fs_lib,
        override_options: ['cpp_std=' + std, 'werror=true'],
        dependencies: thread_dep,
      ),
    )
  endforeach

  # The layer, adapters/include/yace/xnvme_fs.hpp, is what yace generates from libxnvme_fs.h now; skipped where no
  # yace that can generate it is installed, which is everywhere until yace releases what the script names
  test(
    'cpp, the generated layer is what yace generates',
    find_program('sh'),
    args: [meson.project_source_root() / 'adapters' / 'scripts' / 'generate-cpp.sh', '--check'],
  )
endif
