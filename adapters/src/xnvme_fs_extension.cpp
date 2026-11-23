// SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
// SPDX-FileCopyrightText: Stichting DuckDB Foundation
//
// SPDX-License-Identifier: MIT

#define DUCKDB_EXTENSION_MAIN

#include "xnvme_fs_extension.hpp"

#include "xnvme_fs_filesystem.hpp"
#include "duckdb.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	auto &config = DBConfig::GetConfig(db);

	config.AddExtensionOption(
	    "xnvme_fs_socket_dir",
	    "Directory holding the sockets of the pools' servers, <pool>.sock; defaults to XNVME_FS_SOCKET_DIR",
	    LogicalType::VARCHAR, Value(xnvme_fs::DefaultSocketDir()));
	config.AddExtensionOption(
	    "xnvme_fs_backend",
	    "xNVMe backend the pool members are opened with: upcie, io_uring, io_uring_direct or "
	    "io_uring_cmd; defaults to XNVME_FS_BACKEND",
	    LogicalType::VARCHAR, Value(xnvme_fs::DefaultBackend()), [](ClientContext &, SetScope, Value &parameter) {
		    // Said when the setting is set: a pool is connected to at its first use
		    auto backend = parameter.ToString();
		    if (backend != "upcie" && backend != "io_uring" && backend != "io_uring_direct" &&
		        backend != "io_uring_cmd") {
			    throw InvalidInputException("xnvme fs: backend '%s' is not available; 'upcie', 'io_uring', "
			                                "'io_uring_direct' and 'io_uring_cmd' are",
			                                backend);
		    }
	    });

	config.AddExtensionOption("xnvme_fs_homi_id",
	                          "HOMI id of the server holding the pool's controllers under uPCIe; defaults to "
	                          "XNVME_FS_HOMI_ID",
	                          LogicalType::UINTEGER, Value::UINTEGER(xnvme_fs::DefaultHomiId()));

	FileSystem::GetFileSystem(db).RegisterSubSystem(make_uniq<xnvme_fs::XnvmeFsFileSystem>());
	xnvme_fs::RegisterPoolFunctions(loader);
}

void XnvmeFsExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string XnvmeFsExtension::Name() {
	return "xnvme_fs";
}

std::string XnvmeFsExtension::Version() const {
#ifdef EXT_VERSION_XNVME_FS
	return EXT_VERSION_XNVME_FS;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(xnvme_fs, loader) {
	duckdb::LoadInternal(loader);
}
}
