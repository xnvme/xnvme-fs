// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
// SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
//
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <stdexcept>
#include <string>

namespace xnvme_fs {

//! A failure carrying an errno value, so it crosses the metadata socket and reaches an engine as the same code.
class Error : public std::runtime_error {
public:
	Error(int code_p, const std::string &message) : std::runtime_error(message), code(code_p) {
	}
	int Code() const {
		return code;
	}

private:
	int code;
};

std::string Format(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

[[noreturn]] void Fail(int code, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

} // namespace xnvme_fs
