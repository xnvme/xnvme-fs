// SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
//
// SPDX-License-Identifier: MIT

#include "pool_connection.hpp"

#include "duckdb/common/exception.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace duckdb {
namespace xnvme_fs {

void Raise(const std::system_error &err) {
	switch (err.code().value()) {
	case EINVAL:
	case ENAMETOOLONG:
	case ENOTSUP:
		throw InvalidInputException(err.what());
	default:
		throw IOException(err.what());
	}
}

string DefaultSocketDir() {
	auto env = getenv("XNVME_FS_SOCKET_DIR");
	return env && *env ? string(env) : string("/run/xnvme-fs");
}

string DefaultBackend() {
	auto env = getenv("XNVME_FS_BACKEND");
	return env && *env ? string(env) : string("upcie");
}

uint32_t DefaultHomiId() {
	auto env = getenv("XNVME_FS_HOMI_ID");
	return env && *env ? uint32_t(strtoul(env, nullptr, 10)) : 0;
}

static mutex registry_lock;
static unordered_map<string, std::shared_ptr<::xnvme::fs::Session>> registry;

std::shared_ptr<::xnvme::fs::Session> GetSession(const string &pool, optional_ptr<FileOpener> opener) {
	string dir = DefaultSocketDir();
	string backend = DefaultBackend();
	uint32_t homi_id = DefaultHomiId();
	Value value;
	bool configured = false;
	if (FileOpener::TryGetCurrentSetting(opener, "xnvme_fs_socket_dir", value) && !value.IsNull()) {
		dir = value.ToString();
		configured = true;
	}
	if (FileOpener::TryGetCurrentSetting(opener, "xnvme_fs_backend", value) && !value.IsNull()) {
		backend = value.ToString();
	}
	if (FileOpener::TryGetCurrentSetting(opener, "xnvme_fs_homi_id", value) && !value.IsNull()) {
		homi_id = value.GetValue<uint32_t>();
	}

	lock_guard<mutex> guard(registry_lock);
	auto it = registry.find(pool);
	if (it != registry.end() && it->second->Alive() && (!configured || it->second->socket_dir == dir)) {
		return it->second;
	}
	try {
		auto session = std::make_shared<::xnvme::fs::Session>(pool, dir, backend, homi_id);
		registry[pool] = session;
		return session;
	} catch (std::system_error &err) {
		Raise(err);
	}
}

} // namespace xnvme_fs
} // namespace duckdb
