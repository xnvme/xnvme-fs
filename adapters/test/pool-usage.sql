-- SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
--
-- SPDX-License-Identifier: MIT
--
-- The pool 'ci's use after a group of DuckDB's tests, which leave their databases there.
LOAD xnvme_fs;
SELECT member_index, extents, free_extents, files FROM xnvme_fs_pool_info('ci');
