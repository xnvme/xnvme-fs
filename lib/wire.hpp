// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
// SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
//
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "error.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace xnvme_fs {

//! Little-endian encoding shared by the extent map table and the metadata socket.
class Writer {
public:
	void U8(uint8_t v) {
		buf.push_back(v);
	}
	void U16(uint16_t v) {
		Put(v, 2);
	}
	void U32(uint32_t v) {
		Put(v, 4);
	}
	void U64(uint64_t v) {
		Put(v, 8);
	}
	void I32(int32_t v) {
		Put(uint32_t(v), 4);
	}
	void Str(const std::string &s) {
		U32(uint32_t(s.size()));
		buf.insert(buf.end(), s.begin(), s.end());
	}
	void U64s(const std::vector<uint64_t> &v) {
		U32(uint32_t(v.size()));
		for (auto x : v) {
			U64(x);
		}
	}

	std::vector<uint8_t> buf;

private:
	void Put(uint64_t v, int n) {
		for (int i = 0; i < n; i++) {
			buf.push_back(uint8_t(v >> (8 * i)));
		}
	}
};

//! Throws Error(EPROTO) on reading past the end, so a short or corrupt message never reads stray memory.
class Reader {
public:
	Reader(const uint8_t *data, size_t len) : p(data), end(data + len) {
	}

	uint8_t U8() {
		return uint8_t(Get(1));
	}
	uint16_t U16() {
		return uint16_t(Get(2));
	}
	uint32_t U32() {
		return uint32_t(Get(4));
	}
	uint64_t U64() {
		return Get(8);
	}
	int32_t I32() {
		return int32_t(uint32_t(Get(4)));
	}
	std::string Str() {
		auto n = U32();
		Need(n);
		std::string s(reinterpret_cast<const char *>(p), n);
		p += n;
		return s;
	}
	std::vector<uint64_t> U64s() {
		auto n = U32();
		Need(uint64_t(n) * 8);
		std::vector<uint64_t> v(n);
		for (auto &x : v) {
			x = U64();
		}
		return v;
	}
	size_t Remaining() const {
		return size_t(end - p);
	}

private:
	void Need(uint64_t n) {
		if (n > uint64_t(end - p)) {
			throw Error(EPROTO, "xnvme fs: truncated message");
		}
	}
	uint64_t Get(int n) {
		Need(n);
		uint64_t v = 0;
		for (int i = n - 1; i >= 0; i--) {
			v = (v << 8) | p[i];
		}
		p += n;
		return v;
	}

	const uint8_t *p;
	const uint8_t *end;
};

uint32_t Crc32(const uint8_t *data, size_t n, uint32_t crc = 0);

} // namespace xnvme_fs
