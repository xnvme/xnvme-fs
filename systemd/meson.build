# SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
# SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
#
# SPDX-License-Identifier: BSD-3-Clause

fs_unitdir = get_option('systemd_unitdir')
systemd_opt = get_option('systemd')

if fs_unitdir == ''
  # Nothing is looked for where the feature is disabled
  systemd_dep = dependency('systemd', required: systemd_opt)
  if systemd_dep.found()
    # pkg-config answers with an absolute path that ignores this build's prefix
    fs_unitdir = systemd_dep.get_variable(
      pkgconfig: 'systemdsystemunitdir',
      pkgconfig_define: ['prefix', get_option('prefix')],
    )
  endif
endif

if systemd_opt.allowed() and fs_unitdir != ''
  unit_conf = configuration_data()
  # Absolute: systemd resolves neither PATH nor a relative ExecStart or EnvironmentFile
  unit_conf.set('libexecdir', xnvme_libexecdir)
  unit_conf.set('sysconfdir', get_option('prefix') / get_option('sysconfdir'))

  fs_unit = configure_file(
    input: 'xnvme-fs@.service.in',
    output: 'xnvme-fs@.service',
    configuration: unit_conf,
    install: true,
    install_dir: fs_unitdir,
  )

  install_data(
    'fs.conf.example',
    install_dir: get_option('sysconfdir') / 'xnvme',
  )

  # Nothing else in the build reads the unit
  test(
    'systemd, xnvme-fs@.service',
    python3,
    args: [
      files('unit_check.py'),
      fs_unit,
      'ci',
      unit_conf.get('libexecdir'),
      unit_conf.get('sysconfdir'),
      'xnvme-fs',
      'serve',
    ],
    suite: 'systemd',
  )

  # Its ExecStart is a shell script, which systemd-analyze does not run
  test(
    'systemd, xnvme-fs@.service, ExecStart',
    python3,
    args: [files('fs_unit_start.py'), fs_unit, unit_conf.get('libexecdir')],
    suite: 'systemd',
  )
else
  fs_unitdir = ''
endif
