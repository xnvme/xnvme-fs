# SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
#
# SPDX-License-Identifier: MIT

project = "xnvme fs"
author = "Simon A. F. Lund"
copyright = "Simon A. F. Lund"

extensions = ["myst_parser"]

myst_enable_extensions = ["colon_fence", "deflist"]
myst_heading_anchors = 3

exclude_patterns = ["_build", ".venv", "README.md"]

html_theme = "sphinx_book_theme"
html_title = "xnvme fs"
html_theme_options = {
    "repository_url": "https://github.com/xnvme/xnvme-fs",
    "use_repository_button": True,
    "path_to_docs": "docs",
}
