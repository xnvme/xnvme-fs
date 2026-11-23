<!--
SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>

SPDX-License-Identifier: CC-BY-4.0
-->

# xnvme fs documentation

The documentation is a Sphinx project in MyST Markdown. With Python 3 and its
`venv` module installed, `make` in this directory installs Sphinx into
`.venv` and builds the HTML into `_build/html`. `make clean` removes both
directories.

`make` runs `check-api.sh` before Sphinx. `check-api.sh` fails when `api.md`
does not name a function, struct or macro that `include/libxnvme_fs.h`
declares.
