// SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
//
// SPDX-License-Identifier: MIT
//
// RocksDB's FileSystem on an xnvme fs pool, reached by URI as "xnvme-fs://<pool>", e.g.
// db_bench --fs_uri=xnvme-fs://pool0. Metadata goes through the socket of the pool's server and data through the
// pool's members on the calling thread's queues, through libxnvme-fs as the DuckDB adapter does. RocksDB's paths
// name files in the pool: "/db/000012.sst" is the pool's "db/000012.sst".

#include "xnvme_fs.hpp"
#include "rocksdb/file_system.h"
#include "rocksdb/utilities/object_registry.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <mutex>
#include <set>
#include <vector>

namespace xnvme_fs_rocksdb {

using namespace ROCKSDB_NAMESPACE;

static IOStatus ToStatus(const std::system_error &err) {
	switch (err.code().value()) {
	case ENOENT:
		return IOStatus::PathNotFound(err.what());
	case ENOSPC:
		return IOStatus::NoSpace(err.what());
	case ENOTSUP:
		return IOStatus::NotSupported(err.what());
	case EINVAL:
	case ENAMETOOLONG:
		return IOStatus::InvalidArgument(err.what());
	default:
		return IOStatus::IOError(err.what());
	}
}

//! Runs 'f', turning a failed call of xnvme fs into the IOStatus RocksDB expects.
template <class F>
static IOStatus Guard(F f) {
	try {
		return f();
	} catch (std::system_error &err) {
		return ToStatus(err);
	}
}

//! A RocksDB path as a name in the pool: no leading, doubled or trailing slashes, no "." components.
static std::string Name(const std::string &path) {
	std::string name;
	size_t i = 0;
	while (i < path.size()) {
		while (i < path.size() && path[i] == '/') {
			i++;
		}
		size_t j = path.find('/', i);
		auto part = path.substr(i, j == std::string::npos ? std::string::npos : j - i);
		if (!part.empty() && part != ".") {
			name += (name.empty() ? "" : "/") + part;
		}
		if (j == std::string::npos) {
			break;
		}
		i = j;
	}
	return name;
}

class SequentialFile : public FSSequentialFile {
public:
	explicit SequentialFile(std::shared_ptr<::xnvme::fs::File> file_p) : file(std::move(file_p)) {
	}
	IOStatus Read(size_t n, const IOOptions &, Slice *result, char *scratch, IODebugContext *) override {
		return Guard([&]() {
			auto got = file->Read(scratch, n, pos);
			pos += got;
			*result = Slice(scratch, got);
			return IOStatus::OK();
		});
	}
	IOStatus Skip(uint64_t n) override {
		pos = std::min(pos + n, file->Size());
		return IOStatus::OK();
	}

private:
	std::shared_ptr<::xnvme::fs::File> file;
	uint64_t pos = 0;
};

//! A read RocksDB started with ReadAsync: the request carrying it, and what to call when Poll reaps it.
struct AsyncRead {
	::xnvme_fs::req request = ::xnvme_fs::req_new();
	//! A copy: callers pass a request on their stack, gone by the time Poll calls them back with this one.
	FSReadRequest req;
	std::function<void(FSReadRequest &, void *)> cb;
	void *cb_arg;
};

//! Waits for a read's request, and fills in the read's result from it.
static void Done(FSReadRequest &req, ::xnvme_fs::req &request) {
	try {
		req.result = Slice(req.scratch, request.wait());
		req.status = IOStatus::OK();
	} catch (std::system_error &err) {
		req.result = Slice(req.scratch, 0);
		req.status = IOStatus::IOError(err.what());
	}
}

//! Waits for a request whose result is reported elsewhere, or to nobody.
static void WaitOut(::xnvme_fs::req &request) {
	try {
		request.wait();
	} catch (std::system_error &) { // NOLINT: Done reports the failure where the read is reported
	}
}

class RandomAccessFile : public FSRandomAccessFile {
public:
	explicit RandomAccessFile(std::shared_ptr<::xnvme::fs::File> file_p) : file(std::move(file_p)) {
	}
	IOStatus Read(uint64_t offset, size_t n, const IOOptions &, Slice *result, char *scratch,
	              IODebugContext *) const override {
		return Guard([&]() {
			auto got = file->Read(scratch, n, offset);
			*result = Slice(scratch, got);
			return IOStatus::OK();
		});
	}
	//! Every read of the batch is in flight at once, spread over the members' queues, before any is waited for.
	IOStatus MultiRead(FSReadRequest *reqs, size_t num_reqs, const IOOptions &, IODebugContext *) override {
		return Guard([&]() {
			// All made before any is started: a request in flight must be waited for before it is freed
			std::vector<::xnvme_fs::req> requests;
			requests.reserve(num_reqs);
			for (size_t i = 0; i < num_reqs; i++) {
				requests.push_back(::xnvme_fs::req_new());
			}
			std::vector<bool> started(num_reqs, false);
			for (size_t i = 0; i < num_reqs; i++) {
				try {
					file->Handle().submit(requests[i], 0, reqs[i].scratch, reqs[i].len, reqs[i].offset);
					started[i] = true;
				} catch (std::system_error &err) {
					reqs[i].result = Slice(reqs[i].scratch, 0);
					reqs[i].status = IOStatus::IOError(err.what());
				}
			}
			for (size_t i = 0; i < num_reqs; i++) {
				if (started[i]) {
					Done(reqs[i], requests[i]);
				}
			}
			return IOStatus::OK();
		});
	}
	IOStatus ReadAsync(FSReadRequest &req, const IOOptions &, std::function<void(FSReadRequest &, void *)> cb,
	                   void *cb_arg, void **io_handle, IOHandleDeleter *del_fn, IODebugContext *) override {
		return Guard([&]() {
			std::unique_ptr<AsyncRead> a(new AsyncRead());
			a->req.offset = req.offset;
			a->req.len = req.len;
			a->req.scratch = req.scratch;
			a->cb = std::move(cb);
			a->cb_arg = cb_arg;
			file->Handle().submit(a->request, 0, req.scratch, req.len, req.offset);
			*io_handle = a.release();
			*del_fn = [](void *h) {
				delete static_cast<AsyncRead *>(h);
			};
			return IOStatus::OK();
		});
	}
	size_t GetUniqueId(char *, size_t) const override {
		return 0;
	}

private:
	std::shared_ptr<::xnvme::fs::File> file;
};

//! A file opened with use_mmap_reads, which the pool cannot map: its contents read into memory at open, and reads
//! answered by slices of them, as from a mapping. RocksDB maps only files it has finished writing, and PlainTable
//! reads with no scratch buffer, relying on it.
class MappedFile : public FSRandomAccessFile {
public:
	explicit MappedFile(const std::shared_ptr<::xnvme::fs::File> &file) : data(file->Size()) {
		data.resize(file->Read(data.data(), data.size(), 0));
	}
	IOStatus Read(uint64_t offset, size_t n, const IOOptions &, Slice *result, char *,
	              IODebugContext *) const override {
		offset = std::min<uint64_t>(offset, data.size());
		*result = Slice(data.data() + offset, std::min<uint64_t>(n, data.size() - offset));
		return IOStatus::OK();
	}

private:
	std::vector<char> data;
};

class WritableFile : public FSWritableFile {
public:
	explicit WritableFile(std::shared_ptr<::xnvme::fs::File> file_p) : file(std::move(file_p)) {
		size = this->file->Size();
	}
	using FSWritableFile::Append;
	using FSWritableFile::PositionedAppend;
	IOStatus Append(const Slice &data, const IOOptions &, IODebugContext *) override {
		return Guard([&]() {
			file->Write(data.data(), data.size(), size);
			size += data.size();
			return IOStatus::OK();
		});
	}
	IOStatus PositionedAppend(const Slice &data, uint64_t offset, const IOOptions &, IODebugContext *) override {
		return Guard([&]() {
			file->Write(data.data(), data.size(), offset);
			size = std::max<uint64_t>(size, offset + data.size());
			return IOStatus::OK();
		});
	}
	IOStatus Truncate(uint64_t new_size, const IOOptions &, IODebugContext *) override {
		return Guard([&]() {
			file->Truncate(new_size);
			size = new_size;
			return IOStatus::OK();
		});
	}
	// Like a page cache, XNVME_FS_APPEND_BUFFER holds appends past RocksDB's flush, until a sync or the close
	IOStatus Flush(const IOOptions &, IODebugContext *) override {
		return IOStatus::OK();
	}
	IOStatus Sync(const IOOptions &, IODebugContext *) override {
		return Guard([&]() {
			file->Sync();
			return IOStatus::OK();
		});
	}
	IOStatus Close(const IOOptions &, IODebugContext *) override {
		auto closing = std::move(file);
		if (!closing) {
			return IOStatus::OK();
		}
		return Guard([&]() {
			closing->Close();
			return IOStatus::OK();
		});
	}
	uint64_t GetFileSize(const IOOptions &, IODebugContext *) override {
		return size;
	}
	bool IsSyncThreadSafe() const override {
		return true;
	}

private:
	std::shared_ptr<::xnvme::fs::File> file;
	uint64_t size;
};

//! A file's commit, a rename and a removal each reach the drive before they return, so a directory has nothing of
//! its own left to make durable.
class Directory : public FSDirectory {
public:
	IOStatus Fsync(const IOOptions &, IODebugContext *) override {
		return IOStatus::OK();
	}
	IOStatus Close(const IOOptions &, IODebugContext *) override {
		return IOStatus::OK();
	}
};

//! Names locked in this process, as "<pool>/<name>". The server refuses a second process the open LOCK file, but not a
//! second open in the session that holds it, so a second lock in one process is refused here, as RocksDB's POSIX file
//! system does for fcntl locks.
static std::mutex locked_mutex;
static std::set<std::string> locked;

//! Holds the LOCK file open, and its name in 'locked'.
class Lock : public FileLock {
public:
	Lock(std::string key_p, std::shared_ptr<::xnvme::fs::File> file_p)
	    : key(std::move(key_p)), file(std::move(file_p)) {
	}
	~Lock() override {
		std::lock_guard<std::mutex> guard(locked_mutex);
		locked.erase(key);
	}

private:
	std::string key;
	std::shared_ptr<::xnvme::fs::File> file;
};

class XnvmeFsFileSystem : public FileSystem {
public:
	explicit XnvmeFsFileSystem(std::string pool_p) : pool(std::move(pool_p)) {
	}
	static const char *kClassName() {
		return "XnvmeFsFileSystem";
	}
	const char *Name() const override {
		return kClassName();
	}

	IOStatus NewSequentialFile(const std::string &fname, const FileOptions &, std::unique_ptr<FSSequentialFile> *result,
	                           IODebugContext *) override {
		return Guard([&]() {
			result->reset(new SequentialFile(Session()->Open(xnvme_fs_rocksdb::Name(fname), 0)));
			return IOStatus::OK();
		});
	}
	IOStatus NewRandomAccessFile(const std::string &fname, const FileOptions &options,
	                             std::unique_ptr<FSRandomAccessFile> *result, IODebugContext *) override {
		return Guard([&]() {
			auto file = Session()->Open(xnvme_fs_rocksdb::Name(fname), 0);
			if (options.use_mmap_reads) {
				result->reset(new MappedFile(file));
			} else {
				result->reset(new RandomAccessFile(std::move(file)));
			}
			return IOStatus::OK();
		});
	}
	IOStatus NewWritableFile(const std::string &fname, const FileOptions &, std::unique_ptr<FSWritableFile> *result,
	                         IODebugContext *) override {
		return Guard([&]() {
			auto flags = XNVME_FS_O_CREATE | XNVME_FS_O_TRUNC;
			result->reset(new WritableFile(Session()->Open(xnvme_fs_rocksdb::Name(fname), flags)));
			return IOStatus::OK();
		});
	}
	IOStatus ReopenWritableFile(const std::string &fname, const FileOptions &, std::unique_ptr<FSWritableFile> *result,
	                            IODebugContext *) override {
		return Guard([&]() {
			result->reset(new WritableFile(Session()->Open(xnvme_fs_rocksdb::Name(fname), XNVME_FS_O_CREATE)));
			return IOStatus::OK();
		});
	}
	IOStatus NewDirectory(const std::string &, const IOOptions &, std::unique_ptr<FSDirectory> *result,
	                      IODebugContext *) override {
		result->reset(new Directory());
		return IOStatus::OK();
	}

	IOStatus FileExists(const std::string &fname, const IOOptions &, IODebugContext *) override {
		return Guard([&]() {
			auto kind = Session()->Stat(xnvme_fs_rocksdb::Name(fname)).kind;
			return kind ? IOStatus::OK() : IOStatus::NotFound(fname);
		});
	}
	IOStatus GetChildren(const std::string &dir, const IOOptions &, std::vector<std::string> *result,
	                     IODebugContext *) override {
		auto status = Guard([&]() {
			result->clear();
			for (auto &e : Session()->List(xnvme_fs_rocksdb::Name(dir))) {
				result->push_back(e.name);
			}
			return IOStatus::OK();
		});
		// RocksDB tells a missing directory from a failed listing by NotFound, as its POSIX file system reports it
		return status.IsPathNotFound() ? IOStatus::NotFound(dir) : status;
	}
	IOStatus DeleteFile(const std::string &fname, const IOOptions &, IODebugContext *) override {
		return Guard([&]() {
			Session()->Remove(xnvme_fs_rocksdb::Name(fname));
			return IOStatus::OK();
		});
	}
	IOStatus Truncate(const std::string &fname, size_t size, const IOOptions &, IODebugContext *) override {
		return Guard([&]() {
			Session()->Open(xnvme_fs_rocksdb::Name(fname), 0)->Truncate(size);
			return IOStatus::OK();
		});
	}
	IOStatus CreateDir(const std::string &dirname, const IOOptions &o, IODebugContext *d) override {
		auto status = Guard([&]() {
			auto kind = Session()->Stat(xnvme_fs_rocksdb::Name(dirname)).kind;
			return kind ? IOStatus::IOError(dirname + " exists") : IOStatus::OK();
		});
		return status.ok() ? CreateDirIfMissing(dirname, o, d) : status;
	}
	IOStatus CreateDirIfMissing(const std::string &dirname, const IOOptions &, IODebugContext *) override {
		return Guard([&]() {
			auto name = xnvme_fs_rocksdb::Name(dirname);
			if (!name.empty()) {
				Session()->Mkdir(name);
			}
			return IOStatus::OK();
		});
	}
	IOStatus DeleteDir(const std::string &dirname, const IOOptions &, IODebugContext *) override {
		return Guard([&]() {
			Session()->Rmdir(xnvme_fs_rocksdb::Name(dirname));
			return IOStatus::OK();
		});
	}
	IOStatus GetFileSize(const std::string &fname, const IOOptions &, uint64_t *file_size, IODebugContext *) override {
		return Guard([&]() {
			auto st = Session()->Stat(xnvme_fs_rocksdb::Name(fname));
			if (!st.kind) {
				return IOStatus::PathNotFound(fname);
			}
			*file_size = st.size;
			return IOStatus::OK();
		});
	}
	// The pool keeps no times
	IOStatus GetFileModificationTime(const std::string &, const IOOptions &, uint64_t *file_mtime,
	                                 IODebugContext *) override {
		*file_mtime = 0;
		return IOStatus::OK();
	}
	IOStatus RenameFile(const std::string &src, const std::string &target, const IOOptions &,
	                    IODebugContext *) override {
		return Guard([&]() {
			Session()->Rename(xnvme_fs_rocksdb::Name(src), xnvme_fs_rocksdb::Name(target));
			return IOStatus::OK();
		});
	}
	IOStatus LockFile(const std::string &fname, const IOOptions &, FileLock **held, IODebugContext *) override {
		auto name = xnvme_fs_rocksdb::Name(fname);
		auto key = pool + "/" + name;
		{
			std::lock_guard<std::mutex> guard(locked_mutex);
			if (!locked.insert(key).second) {
				return IOStatus::IOError("lock " + fname + ": already held by this process");
			}
		}
		auto status = Guard([&]() {
			*held = new Lock(key, Session()->Open(name, XNVME_FS_O_CREATE));
			return IOStatus::OK();
		});
		if (!status.ok()) {
			std::lock_guard<std::mutex> guard(locked_mutex);
			locked.erase(key);
		}
		return status;
	}
	IOStatus UnlockFile(FileLock *held, const IOOptions &, IODebugContext *) override {
		delete held;
		return IOStatus::OK();
	}
	//! TEST_TMPDIR or /tmp, as RocksDB's POSIX file system has it: some of RocksDB's tests take this directory to
	//! Env::Default() as well, so it must exist on the host too.
	IOStatus GetTestDirectory(const IOOptions &, std::string *path, IODebugContext *) override {
		auto dir = std::getenv("TEST_TMPDIR");
		*path = dir && *dir ? dir : "/tmp";
		return IOStatus::OK();
	}
	IOStatus GetAbsolutePath(const std::string &db_path, const IOOptions &, std::string *output_path,
	                         IODebugContext *) override {
		*output_path = "/" + xnvme_fs_rocksdb::Name(db_path);
		return IOStatus::OK();
	}
	IOStatus IsDirectory(const std::string &path, const IOOptions &, bool *is_dir, IODebugContext *) override {
		return Guard([&]() {
			auto st = Session()->Stat(xnvme_fs_rocksdb::Name(path));
			if (!st.kind) {
				return IOStatus::PathNotFound(path);
			}
			*is_dir = st.kind == XNVME_FS_KIND_DIRECTORY;
			return IOStatus::OK();
		});
	}
	//! Reaps reads started with ReadAsync, calling RocksDB back for each, until at least 'min_completions' are done.
	IOStatus Poll(std::vector<void *> &io_handles, size_t min_completions) override {
		return Guard([&]() {
			size_t done = 0;
			std::vector<bool> reported(io_handles.size(), false);
			while (done < std::min(min_completions, io_handles.size())) {
				size_t before = done;
				for (size_t i = 0; i < io_handles.size(); i++) {
					auto a = static_cast<AsyncRead *>(io_handles[i]);
					if (reported[i] || !a->request.poll()) {
						continue;
					}
					reported[i] = true;
					done++;
					Done(a->req, a->request);
					a->cb(a->req, a->cb_arg);
				}
				// A pass that reaped nothing waits on the first outstanding read, which paces the polling
				for (size_t i = 0; done == before && i < io_handles.size(); i++) {
					if (!reported[i]) {
						WaitOut(static_cast<AsyncRead *>(io_handles[i])->request);
						break;
					}
				}
			}
			return IOStatus::OK();
		});
	}
	//! Requests on a device cannot be withdrawn, so aborting waits them out, without calling back.
	IOStatus AbortIO(std::vector<void *> &io_handles) override {
		for (auto h : io_handles) {
			WaitOut(static_cast<AsyncRead *>(h)->request);
		}
		return IOStatus::OK();
	}

private:
	//! The pool's session, made on first use, so a FileSystem can be made before the server is asked anything.
	std::shared_ptr<::xnvme::fs::Session> Session() {
		std::lock_guard<std::mutex> guard(lock);
		if (!session || !session->Alive()) {
			auto dir = getenv("XNVME_FS_SOCKET_DIR");
			auto homi = getenv("XNVME_FS_HOMI_ID");
			session = std::make_shared<::xnvme::fs::Session>(pool, dir && *dir ? dir : "/run/xnvme-fs", "",
			                                                 homi && *homi ? uint32_t(strtoul(homi, nullptr, 10)) : 0);
		}
		return session;
	}

	std::string pool;
	std::mutex lock;
	std::shared_ptr<::xnvme::fs::Session> session;
};

} // namespace xnvme_fs_rocksdb

//! Registers "xnvme-fs://<pool>" with RocksDB's object registry; RocksDB's build calls it for plugins it links in, and
//! declares it extern "C".
extern "C" int RegisterXnvmeFs(ROCKSDB_NAMESPACE::ObjectLibrary &library, const std::string &) {
	using ROCKSDB_NAMESPACE::FileSystem;
	using ROCKSDB_NAMESPACE::ObjectLibrary;
	library.AddFactory<FileSystem>(ObjectLibrary::PatternEntry("xnvme-fs", false).AddSeparator("://", false),
	                               [](const std::string &uri, std::unique_ptr<FileSystem> *guard, std::string *) {
		                               guard->reset(
		                                   new xnvme_fs_rocksdb::XnvmeFsFileSystem(uri.substr(strlen("xnvme-fs://"))));
		                               return guard->get();
	                               });
	return 1;
}
