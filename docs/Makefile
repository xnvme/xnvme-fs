# SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
#
# SPDX-License-Identifier: MIT
#
# The documentation as HTML in _build/html, with Sphinx in a virtualenv of its own in .venv. check-api.sh first fails
# where api.md does not name a declaration of libxnvme_fs.h.

VENV := .venv

.PHONY: html clean

html: $(VENV)/bin/sphinx-build
	sh check-api.sh
	$(VENV)/bin/sphinx-build -W -b html . _build/html

$(VENV)/bin/sphinx-build: requirements.txt
	python3 -m venv $(VENV)
	$(VENV)/bin/pip install -q -r requirements.txt
	touch $@

clean:
	rm -rf _build $(VENV)
