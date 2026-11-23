// SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
//
// SPDX-License-Identifier: MIT

#include "xnvme_fs_filesystem.hpp"

#include "write_behind.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/timestamp.hpp"

#include <cerrno>

namespace duckdb {
namespace xnvme_fs {

XnvmeFsPath XnvmeFsPath::Parse(const string &path) {
	if (!StringUtil::StartsWith(path, PREFIX)) {
		throw InvalidInputException("xnvme fs: '%s' is not an xnvme-fs:// path", path);
	}
	auto rest = path.substr(strlen(PREFIX));
	auto slash = rest.find('/');
	XnvmeFsPath p;
	p.pool = slash == string::npos ? rest : rest.substr(0, slash);
	if (p.pool.empty()) {
		throw InvalidInputException("xnvme fs: '%s' names no pool", path);
	}
	if (slash != string::npos) {
		// Joined paths may carry doubled or trailing slashes; the pool's names have neither.
		for (auto c : rest.substr(slash + 1)) {
			if (c == '/' && (p.name.empty() || p.name.back() == '/')) {
				continue;
			}
			p.name += c;
		}
		if (!p.name.empty() && p.name.back() == '/') {
			p.name.pop_back();
		}
	}
	return p;
}

XnvmeFsFileHandle::XnvmeFsFileHandle(FileSystem &fs, const string &path, FileOpenFlags flags,
                                     std::shared_ptr<::xnvme::fs::File> file_p)
    : FileHandle(fs, path, flags), file(std::move(file_p)), behind(WritesBehind(path)) {
}

XnvmeFsFileHandle::~XnvmeFsFileHandle() {
	try {
		XnvmeFsFileHandle::Close();
	} catch (...) { // NOLINT: a destructor cannot report; DuckDB closes explicitly where it can
	}
}

void XnvmeFsFileHandle::Close() {
	auto closing = std::move(file);
	// A write behind holds the file as well, and closes it when the last one is reaped
	if (closing && closing.use_count() == 1) {
		try {
			closing->Close();
		} catch (std::system_error &err) {
			throw IOException("%s", err.what());
		}
	}
}

static XnvmeFsFileHandle &Handle(FileHandle &handle) {
	auto &h = handle.Cast<XnvmeFsFileHandle>();
	if (!h.file) {
		throw IOException("xnvme fs: '%s' is closed", h.path);
	}
	return h;
}

bool XnvmeFsFileSystem::CanHandleFile(const string &fpath) {
	return StringUtil::StartsWith(fpath, PREFIX);
}

unique_ptr<FileHandle> XnvmeFsFileSystem::OpenFile(const string &path, FileOpenFlags flags,
                                                   optional_ptr<FileOpener> opener) {
	if (flags.Compression() != FileCompressionType::UNCOMPRESSED &&
	    flags.Compression() != FileCompressionType::AUTO_DETECT) {
		throw NotImplementedException("xnvme fs: compressed files are not supported: '%s'", path);
	}
	Reap(false);
	auto p = XnvmeFsPath::Parse(path);
	auto session = GetSession(p.pool, opener);
	if (flags.ReturnNullIfExists() || flags.ReturnNullIfNotExists()) {
		try {
			auto kind = session->Stat(p.name).kind;
			if (flags.ReturnNullIfExists() && kind != 0) {
				return nullptr;
			}
			if (flags.ReturnNullIfNotExists() && kind == 0) {
				return nullptr;
			}
		} catch (std::system_error &err) {
			Raise(err);
		}
	}
	uint32_t open_flags = 0;
	if (flags.CreateFileIfNotExists() || flags.OverwriteExistingFile()) {
		open_flags |= XNVME_FS_O_CREATE;
	}
	if (flags.OverwriteExistingFile()) {
		open_flags |= XNVME_FS_O_TRUNC;
	}
	if (flags.ExclusiveCreate()) {
		open_flags |= XNVME_FS_O_CREATE | XNVME_FS_O_EXCL;
	}
	std::shared_ptr<::xnvme::fs::File> file;
	try {
		file = session->Open(p.name, open_flags);
	} catch (std::system_error &err) {
		Raise(err);
	}
	return make_uniq<XnvmeFsFileHandle>(*this, path, flags, std::move(file));
}

void XnvmeFsFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	Reap(false);
	auto &h = Handle(handle);
	idx_t got;
	try {
		if (h.behind) {
			Overlay overlay(*h.file, idx_t(nr_bytes), location);
			got = h.file->Read(buffer, idx_t(nr_bytes), location);
			overlay.Apply(buffer);
		} else {
			got = h.file->Read(buffer, idx_t(nr_bytes), location);
		}
	} catch (std::system_error &err) {
		Raise(err);
	}
	if (got < idx_t(nr_bytes)) {
		throw IOException("xnvme fs: read of %llu bytes at %llu is past the end of '%s' (%llu bytes)", nr_bytes,
		                  location, h.path, h.file->Size());
	}
}

void XnvmeFsFileSystem::Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	Reap(false);
	auto &h = Handle(handle);
	try {
		if (h.behind) {
			WriteBehind(h.file, buffer, idx_t(nr_bytes), location);
		} else {
			h.file->Write(buffer, idx_t(nr_bytes), location);
		}
	} catch (std::system_error &err) {
		Raise(err);
	}
}

int64_t XnvmeFsFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	auto &h = Handle(handle);
	auto size = h.file->Size();
	auto n = h.position >= size ? 0 : MinValue<idx_t>(idx_t(nr_bytes), size - h.position);
	if (n > 0) {
		try {
			h.file->Read(buffer, n, h.position);
		} catch (std::system_error &err) {
			Raise(err);
		}
		h.position += n;
	}
	return int64_t(n);
}

int64_t XnvmeFsFileSystem::Write(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	auto &h = Handle(handle);
	if (h.flags.OpenForAppending()) {
		h.position = h.file->Size();
	}
	try {
		h.file->Write(buffer, idx_t(nr_bytes), h.position);
	} catch (std::system_error &err) {
		Raise(err);
	}
	h.position += idx_t(nr_bytes);
	return nr_bytes;
}

int64_t XnvmeFsFileSystem::GetFileSize(FileHandle &handle) {
	return int64_t(Handle(handle).file->Size());
}

timestamp_t XnvmeFsFileSystem::GetLastModifiedTime(FileHandle &handle) {
	// The pool keeps no times; a constant tells a cache that nothing changed behind its back, which holds while
	// one client owns the file.
	return Timestamp::FromEpochSeconds(0);
}

FileType XnvmeFsFileSystem::GetFileType(FileHandle &handle) {
	return FileType::FILE_TYPE_REGULAR;
}

FileMetadata XnvmeFsFileSystem::Stats(FileHandle &handle) {
	FileMetadata meta;
	meta.file_size = GetFileSize(handle);
	meta.last_modification_time = GetLastModifiedTime(handle);
	meta.file_type = FileType::FILE_TYPE_REGULAR;
	return meta;
}

void XnvmeFsFileSystem::Truncate(FileHandle &handle, int64_t new_size) {
	auto &h = Handle(handle);
	if (h.behind) {
		Reap(true);
		// Extents a truncate releases go to other files, so none is released while a write of another thread may
		// still land in it; the space comes back when the temporary file is removed
		if (idx_t(new_size) < h.file->Size() && InFlightPast(*h.file, idx_t(new_size))) {
			return;
		}
	}
	try {
		h.file->Truncate(idx_t(new_size));
	} catch (std::system_error &err) {
		Raise(err);
	}
}

void XnvmeFsFileSystem::FileSync(FileHandle &handle) {
	Reap(true);
	try {
		Handle(handle).file->Sync();
	} catch (std::system_error &err) {
		Raise(err);
	}
}

void XnvmeFsFileSystem::Seek(FileHandle &handle, idx_t location) {
	Handle(handle).position = location;
}

void XnvmeFsFileSystem::Reset(FileHandle &handle) {
	Handle(handle).position = 0;
}

idx_t XnvmeFsFileSystem::SeekPosition(FileHandle &handle) {
	return Handle(handle).position;
}

struct xnvme_fs_stat XnvmeFsFileSystem::StatPath(const string &path, optional_ptr<FileOpener> opener) {
	auto p = XnvmeFsPath::Parse(path);
	try {
		return GetSession(p.pool, opener)->Stat(p.name);
	} catch (std::system_error &err) {
		Raise(err);
	}
}

bool XnvmeFsFileSystem::FileExists(const string &filename, optional_ptr<FileOpener> opener) {
	return StatPath(filename, opener).kind == XNVME_FS_KIND_FILE;
}

bool XnvmeFsFileSystem::DirectoryExists(const string &directory, optional_ptr<FileOpener> opener) {
	return StatPath(directory, opener).kind == XNVME_FS_KIND_DIRECTORY;
}

void XnvmeFsFileSystem::CreateDirectory(const string &directory, optional_ptr<FileOpener> opener) {
	auto p = XnvmeFsPath::Parse(directory);
	if (p.name.empty()) {
		return;
	}
	try {
		GetSession(p.pool, opener)->Mkdir(p.name);
	} catch (std::system_error &err) {
		Raise(err);
	}
}

void XnvmeFsFileSystem::RemoveDirectory(const string &directory, optional_ptr<FileOpener> opener) {
	auto p = XnvmeFsPath::Parse(directory);
	try {
		GetSession(p.pool, opener)->Rmdir(p.name);
	} catch (std::system_error &err) {
		Raise(err);
	}
}

bool XnvmeFsFileSystem::ListFiles(const string &directory, const std::function<void(const string &, bool)> &callback,
                                  FileOpener *opener) {
	auto p = XnvmeFsPath::Parse(directory);
	std::vector<::xnvme::fs::Entry> entries;
	try {
		entries = GetSession(p.pool, opener)->List(p.name);
	} catch (std::system_error &err) {
		if (err.code().value() == ENOENT) {
			return false;
		}
		Raise(err);
	}
	for (auto &e : entries) {
		callback(e.name, e.directory);
	}
	return true;
}

void XnvmeFsFileSystem::MoveFile(const string &source, const string &target, optional_ptr<FileOpener> opener) {
	auto from = XnvmeFsPath::Parse(source);
	auto to = XnvmeFsPath::Parse(target);
	if (from.pool != to.pool) {
		throw IOException("xnvme fs: cannot move '%s' to another pool, '%s'", source, target);
	}
	auto session = GetSession(from.pool, opener);
	try {
		session->Rename(from.name, to.name);
	} catch (std::system_error &err) {
		Raise(err);
	}
}

void XnvmeFsFileSystem::RemoveFile(const string &filename, optional_ptr<FileOpener> opener) {
	auto p = XnvmeFsPath::Parse(filename);
	auto session = GetSession(p.pool, opener);
	try {
		session->Remove(p.name);
	} catch (std::system_error &err) {
		Raise(err);
	}
}

bool XnvmeFsFileSystem::TryRemoveFile(const string &filename, optional_ptr<FileOpener> opener) {
	auto p = XnvmeFsPath::Parse(filename);
	auto session = GetSession(p.pool, opener);
	try {
		session->Remove(p.name);
	} catch (std::system_error &err) {
		if (err.code().value() == ENOENT) {
			return false;
		}
		Raise(err);
	}
	return true;
}

vector<OpenFileInfo> XnvmeFsFileSystem::Glob(const string &path, FileOpener *opener) {
	if (HasGlob(path)) {
		throw NotImplementedException("xnvme fs: globs are not supported: '%s'", path);
	}
	vector<OpenFileInfo> result;
	if (FileExists(path, opener)) {
		result.emplace_back(path);
	}
	return result;
}

} // namespace xnvme_fs
} // namespace duckdb
