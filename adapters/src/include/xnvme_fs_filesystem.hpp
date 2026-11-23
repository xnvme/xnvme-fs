// SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "pool_connection.hpp"

#include "duckdb/common/file_system.hpp"

namespace duckdb {
namespace xnvme_fs {

constexpr const char *PREFIX = "xnvme-fs://";

//! A pool's name and a file's name in it, from "xnvme-fs://<pool>/<name>"; the name is empty for the pool's root.
struct XnvmeFsPath {
	string pool;
	string name;

	static XnvmeFsPath Parse(const string &path);
};

class XnvmeFsFileHandle : public FileHandle {
public:
	XnvmeFsFileHandle(FileSystem &fs, const string &path, FileOpenFlags flags, std::shared_ptr<::xnvme::fs::File> file);
	~XnvmeFsFileHandle() override;
	void Close() override;

	std::shared_ptr<::xnvme::fs::File> file;
	idx_t position = 0;
	//! Writes at an offset return before they reach the drive; see write_behind.hpp.
	bool behind = false;
};

//! Serves "xnvme-fs://<pool>/<name>": metadata through the socket of the pool's server, data straight to the pool's
//! members.
class XnvmeFsFileSystem : public FileSystem {
public:
	unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags,
	                                optional_ptr<FileOpener> opener = nullptr) override;
	void Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	void Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	int64_t Read(FileHandle &handle, void *buffer, int64_t nr_bytes) override;
	int64_t Write(FileHandle &handle, void *buffer, int64_t nr_bytes) override;
	int64_t GetFileSize(FileHandle &handle) override;
	timestamp_t GetLastModifiedTime(FileHandle &handle) override;
	FileType GetFileType(FileHandle &handle) override;
	FileMetadata Stats(FileHandle &handle) override;
	void Truncate(FileHandle &handle, int64_t new_size) override;
	void FileSync(FileHandle &handle) override;
	void Seek(FileHandle &handle, idx_t location) override;
	void Reset(FileHandle &handle) override;
	idx_t SeekPosition(FileHandle &handle) override;
	bool CanSeek() override {
		return true;
	}
	bool OnDiskFile(FileHandle &handle) override {
		return true;
	}

	bool FileExists(const string &filename, optional_ptr<FileOpener> opener = nullptr) override;
	bool DirectoryExists(const string &directory, optional_ptr<FileOpener> opener = nullptr) override;
	void CreateDirectory(const string &directory, optional_ptr<FileOpener> opener = nullptr) override;
	void RemoveDirectory(const string &directory, optional_ptr<FileOpener> opener = nullptr) override;
	bool ListFiles(const string &directory, const std::function<void(const string &, bool)> &callback,
	               FileOpener *opener = nullptr) override;
	void MoveFile(const string &source, const string &target, optional_ptr<FileOpener> opener = nullptr) override;
	void RemoveFile(const string &filename, optional_ptr<FileOpener> opener = nullptr) override;
	bool TryRemoveFile(const string &filename, optional_ptr<FileOpener> opener = nullptr) override;
	vector<OpenFileInfo> Glob(const string &path, FileOpener *opener = nullptr) override;

	bool CanHandleFile(const string &fpath) override;
	//! The base class folds "xnvme-fs://" into "xnvme-fs:/", which no file system claims.
	string CanonicalizePath(const string &path, optional_ptr<FileOpener> opener = nullptr) override {
		return path;
	}
	string PathSeparator(const string &path) override {
		return "/";
	}
	string GetName() const override {
		return "XnvmeFsFileSystem";
	}

private:
	struct xnvme_fs_stat StatPath(const string &path, optional_ptr<FileOpener> opener);
};

void RegisterPoolFunctions(ExtensionLoader &loader);

} // namespace xnvme_fs
} // namespace duckdb
