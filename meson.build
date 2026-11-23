# SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
# SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
#
# SPDX-License-Identifier: BSD-3-Clause

project(
  'xnvme-fs',
  ['c', 'cpp'],
  version: '0.1.0',
  license: 'BSD-3-Clause',
  default_options: [
    'c_std=gnu11',
    'cpp_std=c++17',
    'warning_level=2',
    'buildtype=release',
    'b_ndebug=if-release',
  ],
  meson_version: '>=0.61',
)

if host_machine.system() != 'linux'
  error('xnvme fs is Linux only')
endif

pconf = import('pkgconfig')
python3 = import('python').find_installation()
cc = meson.get_compiler('c')

thread_dep = dependency('threads')
xnvme_dep = dependency('xnvme', method: 'pkg-config')
ufuse_dep = dependency(
  'ufuse',
  version: ['>=0.4.0', '<0.5.0'],
  allow_fallback: true,
  default_options: ['skip-install=true'],
)

# xnvme-fs extends the 'xnvme' front-end, which runs the programs xnvme-<name> of this directory as 'xnvme <name>'
# and lists in its overview the extensions whose <name>.extension is in the extension directory. An xNVMe whose
# xnvme.pc does not name the directories has them at libexec/xnvme and share/xnvme/extensions of its prefix
xnvme_prefix = xnvme_dep.get_variable(pkgconfig: 'prefix')
xnvme_libexecdir = xnvme_dep.get_variable(
  pkgconfig: 'libexecdir',
  default_value: xnvme_prefix / 'libexec' / 'xnvme',
)
xnvme_extensiondir = xnvme_dep.get_variable(
  pkgconfig: 'extensiondir',
  default_value: xnvme_prefix / 'share' / 'xnvme' / 'extensions',
)

# The loader searches a /usr prefix's libdir already, and distributions reject an RPATH to it
fs_rpaths = []
foreach dir : [get_option('prefix') / get_option('libdir'), xnvme_dep.get_variable(pkgconfig: 'libdir')]
  if not dir.startswith('/usr/lib') and not fs_rpaths.contains(dir)
    fs_rpaths += dir
  endif
endforeach
fs_rpath = ':'.join(fs_rpaths)

# Without xnvme_buf_register_dmabuf(), xnvme_fs_register_dmabuf() returns -ENOTSUP
fs_dmabuf = cc.has_function(
  'xnvme_buf_register_dmabuf',
  prefix: '#include <libxnvme.h>',
  dependencies: xnvme_dep,
)
fs_dmabuf = get_option('dmabuf').require(
  fs_dmabuf,
  error_message: 'the xNVMe built against has no xnvme_buf_register_dmabuf()',
).allowed() and fs_dmabuf
if fs_dmabuf
  add_project_arguments('-DXNVME_FS_DMABUF', language: ['c', 'cpp'])
endif

link_args_hardening = get_option('hardening') ? cc.get_supported_arguments(['-znow', '-zrelro']) : []

subdir('include')
subdir('lib')
subdir('tools')
subdir('tests')
subdir('systemd')

summary(
  {
    'xNVMe': xnvme_dep.version() + ' at ' + xnvme_prefix,
    'xnvme-fs, the extension': xnvme_libexecdir,
    'fs.extension': xnvme_extensiondir,
    'mkfs.xnvme-fs and mount.xnvme-fs': get_option('prefix') / get_option('sbindir'),
    'systemd units': fs_unitdir == '' ? 'none' : fs_unitdir,
  },
  section: 'Installs into',
)
summary({'dma-bufs': fs_dmabuf}, section: 'Features', bool_yn: true)
