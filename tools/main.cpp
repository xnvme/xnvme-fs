// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
// SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
//
// SPDX-License-Identifier: BSD-3-Clause
//
// xnvme-fs: formats a pool, and serves its metadata to the engines' adapters on a Unix socket; 'xnvme fs' runs it. Called as
// mkfs.xnvme-fs or mount.xnvme-fs, it takes the command lines mkfs(8) and mount(8) pass their helpers.

#include "server.hpp"

#include "protocol.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace xnvme_fs;

extern "C" int xnvme_fs_cp_main(int argc, char **argv);

static volatile sig_atomic_t stopping = 0;

static void OnSignal(int) {
	stopping = 1;
}

struct Args {
	std::string command;
	std::string pool;
	std::vector<std::string> members;
	Access access;
	std::string socket;
	uint64_t extent_size = DEFAULT_EXTENT_SIZE;
	bool overwrite = false;
	bool trace = false;
	std::string view;
	//! Stop when the view is unmounted, as a mount's daemon does.
	bool stop_with_view = false;
};

//! Prints the usage and exits: asked for with --help, to stdout and 0; for a command line not understood, to stderr
//! and 64.
[[noreturn]] static void Usage(bool asked = false) {
	fprintf(asked ? stdout : stderr,
	        "usage: xnvme fs mkfs  --pool NAME --member MEMBER... ACCESS [--extent-size BYTES] [--overwrite]\n"
	        "       xnvme fs serve --pool NAME --member MEMBER... ACCESS [--socket PATH] [--view DIR] [--trace]\n"
	        "       xnvme fs info  --pool NAME --member MEMBER... ACCESS\n"
	        "       xnvme fs cp POOL in LOCAL NAME | xnvme fs cp POOL out NAME LOCAL\n"
	        "       mkfs.xnvme-fs [-L NAME] [-E EXTENT_SIZE] [-f] [-H N] MEMBER...\n"
	        "       mount.xnvme-fs MEMBER DIR [-o homi=N,socket=PATH,trace]\n"
	        "members are controllers held by 'homi serve' under the HOMI id N, or partition K of a controller's\n"
	        "GPT as BDF/pK, of type a04faa88-135a-4457-b8f6-db8bbaf344b8; sizes take a K, M, G or T suffix; the\n"
	        "socket defaults to /run/xnvme-fs/NAME.sock; the HOMI id to XNVME_FS_HOMI_ID. ACCESS is --homi-id N, or\n"
	        "--backend io_uring, io_uring_direct or io_uring_cmd with members a namespace's block device, or its\n"
	        "character device for io_uring_cmd, through the kernel's nvme driver; the backend defaults to\n"
	        "XNVME_FS_BACKEND, else upcie\n");
	exit(asked ? 0 : 64);
}

static uint64_t ParseSize(const char *s) {
	char *end = nullptr;
	errno = 0;
	unsigned long long v = strtoull(s, &end, 10);
	if (errno || end == s) {
		Usage();
	}
	switch (*end) {
	case 'T':
		v <<= 10;
		// fall through
	case 'G':
		v <<= 10;
		// fall through
	case 'M':
		v <<= 10;
		// fall through
	case 'K':
		v <<= 10;
		end++;
		break;
	default:
		break;
	}
	if (*end) {
		Usage();
	}
	return v;
}

static uint32_t DefaultHomiId() {
	auto env = getenv("XNVME_FS_HOMI_ID");
	return env && *env ? uint32_t(strtoul(env, nullptr, 10)) : 0;
}

static void DefaultSocket(Args &a) {
	if (a.socket.empty()) {
		a.socket = "/run/xnvme-fs/" + a.pool + ".sock";
	}
}

//! mkfs.xnvme-fs [-L NAME] [-E EXTENT_SIZE] [-f] [-H N] MEMBER...
static Args ParseMkfs(int argc, char **argv) {
	Args a;
	a.command = "mkfs";
	a.pool = "xnvme-fs";
	a.access.homi_id = DefaultHomiId();
	for (int i = 1; i < argc; i++) {
		std::string opt = argv[i];
		if (opt == "-f") {
			a.overwrite = true;
		} else if ((opt == "-L" || opt == "-E" || opt == "-H") && i + 1 < argc) {
			const char *val = argv[++i];
			if (opt == "-L") {
				a.pool = val;
			} else if (opt == "-E") {
				a.extent_size = ParseSize(val);
			} else {
				a.access.homi_id = uint32_t(ParseSize(val));
			}
		} else if (!opt.empty() && opt[0] != '-') {
			a.members.push_back(opt);
		} else {
			Usage();
		}
	}
	if (a.members.empty()) {
		Usage();
	}
	return a;
}

//! mount.xnvme-fs MEMBER DIR [-sfnv] [-o OPTIONS]: the pool and its other members come from MEMBER's superblock.
static Args ParseMount(int argc, char **argv) {
	Args a;
	a.command = "mount";
	a.access.homi_id = DefaultHomiId();
	a.stop_with_view = true;
	std::vector<std::string> positional;
	for (int i = 1; i < argc; i++) {
		std::string opt = argv[i];
		if (opt == "-o" && i + 1 < argc) {
			std::string list = argv[++i];
			size_t start = 0;
			while (start <= list.size()) {
				auto comma = list.find(',', start);
				auto o = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
				if (o.rfind("homi=", 0) == 0) {
					a.access.homi_id = uint32_t(ParseSize(o.c_str() + 5));
				} else if (o.rfind("socket=", 0) == 0) {
					a.socket = o.substr(7);
				} else if (o == "trace") {
					a.trace = true;
				}
				// Others, such as ro, rw and the defaults mount(8) passes, mean nothing to a read-only view
				if (comma == std::string::npos) {
					break;
				}
				start = comma + 1;
			}
		} else if (!opt.empty() && opt[0] == '-') {
			// -s, -f, -n, -v, and -t with its type, as mount(8) may pass them
			if (opt == "-t" && i + 1 < argc) {
				i++;
			}
		} else {
			positional.push_back(opt);
		}
	}
	if (positional.size() != 2) {
		Usage();
	}
	a.members.push_back(positional[0]);
	a.view = positional[1];
	return a;
}

static Args Parse(int argc, char **argv) {
	auto slash = strrchr(argv[0], '/');
	std::string self = slash ? slash + 1 : argv[0];
	if (self == "mkfs.xnvme-fs") {
		return ParseMkfs(argc, argv);
	}
	if (self == "mount.xnvme-fs") {
		return ParseMount(argc, argv);
	}
	if (argc < 2) {
		Usage();
	}
	Args a;
	a.command = argv[1];
	a.access.homi_id = DefaultHomiId();
	for (int i = 2; i < argc; i++) {
		std::string opt = argv[i];
		if (opt == "--overwrite") {
			a.overwrite = true;
			continue;
		}
		if (opt == "--trace") {
			a.trace = true;
			continue;
		}
		if (i + 1 >= argc) {
			Usage();
		}
		const char *val = argv[++i];
		if (opt == "--pool") {
			a.pool = val;
		} else if (opt == "--member") {
			a.members.push_back(val);
		} else if (opt == "--backend") {
			a.access.backend = val;
		} else if (opt == "--homi-id") {
			a.access.homi_id = uint32_t(ParseSize(val));
		} else if (opt == "--socket") {
			a.socket = val;
		} else if (opt == "--view") {
			a.view = val;
		} else if (opt == "--extent-size") {
			a.extent_size = ParseSize(val);
		} else {
			Usage();
		}
	}
	if (a.pool.empty() || a.members.empty()) {
		Usage();
	}
	DefaultSocket(a);
	return a;
}

//! Holds an exclusive lock for every drive of every member, so two daemons never format or serve one controller. A
//! drive is a PCI address, not a file to lock, so the lock is a file named for it under /run/lock.
static std::vector<int> LockMembers(const std::vector<std::string> &members) {
	std::vector<std::string> drives;
	for (auto &m : members) {
		for (auto &d : MemberDrives(m)) {
			if (std::find(drives.begin(), drives.end(), d) != drives.end()) {
				Fail(EINVAL, "xnvme fs: '%s' is named twice", d.c_str());
			}
			drives.push_back(d);
		}
	}
	std::vector<int> fds;
	for (auto &m : drives) {
		auto name = m;
		std::replace(name.begin(), name.end(), '/', '-');
		auto path = "/run/lock/xnvme-fs-" + name + ".lock";
		int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
		if (fd < 0) {
			Fail(errno, "xnvme fs: cannot open the lock '%s': %s", path.c_str(), strerror(errno));
		}
		if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
			close(fd);
			Fail(EBUSY, "xnvme fs: '%s' is in use by another 'xnvme fs serve'", m.c_str());
		}
		fds.push_back(fd);
	}
	return fds;
}

static int Listen(const std::string &path) {
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	if (path.size() >= sizeof(addr.sun_path)) {
		Fail(ENAMETOOLONG, "xnvme fs: socket path '%s' is too long", path.c_str());
	}
	memcpy(addr.sun_path, path.data(), path.size());
	auto slash = path.rfind('/');
	if (slash != std::string::npos && slash > 0) {
		mkdir(path.substr(0, slash).c_str(), 0755);
	}
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		Fail(errno, "xnvme fs: cannot create a socket: %s", strerror(errno));
	}
	// A socket nobody answers on is left over from a daemon that died; one that answers is served.
	if (connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) == 0) {
		close(fd);
		Fail(EADDRINUSE, "xnvme fs: '%s' is already served", path.c_str());
	}
	unlink(path.c_str());
	if (bind(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0 || listen(fd, 64) != 0) {
		int saved = errno;
		close(fd);
		Fail(saved, "xnvme fs: cannot listen on '%s': %s", path.c_str(), strerror(saved));
	}
	return fd;
}

static int Serve(const Args &a) {
	auto locks = LockMembers(a.members);
	Server server(Pool::Open(a.pool, a.members, a.access));
	server.trace = a.trace;
	int lfd = Listen(a.socket);
	if (!a.view.empty()) {
		server.OpenView(a.view);
	}

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = OnSignal;
	sigaction(SIGINT, &sa, nullptr);
	sigaction(SIGTERM, &sa, nullptr);
	signal(SIGPIPE, SIG_IGN);

	fprintf(stderr, "xnvme-fs: serving pool '%s' on %s%s%s\n", a.pool.c_str(), a.socket.c_str(),
	        a.view.empty() ? "" : ", viewed at ", a.view.c_str());
	std::vector<int> clients;
	std::vector<uint8_t> request;
	while (!stopping) {
		// The listener, then the view (-1 without one, which poll skips), then the clients
		std::vector<struct pollfd> fds;
		fds.push_back({lfd, POLLIN, 0});
		fds.push_back({server.ViewFd(), POLLIN, 0});
		for (int c : clients) {
			fds.push_back({c, POLLIN, 0});
		}
		if (poll(fds.data(), fds.size(), -1) < 0) {
			if (errno == EINTR) {
				continue;
			}
			fprintf(stderr, "xnvme-fs: poll: %s\n", strerror(errno));
			break;
		}
		if (fds[1].fd >= 0 && fds[1].revents) {
			server.ServeView();
			if (a.stop_with_view && server.ViewFd() < 0) {
				break;
			}
		}
		std::vector<int> gone;
		for (size_t i = 2; i < fds.size(); i++) {
			if (!fds[i].revents) {
				continue;
			}
			int c = fds[i].fd;
			try {
				if (!RecvFrame(c, request)) {
					gone.push_back(c);
					continue;
				}
				auto response = server.Handle(c, request);
				if (!response.empty()) {
					SendFrame(c, response);
				}
			} catch (Error &err) {
				fprintf(stderr, "xnvme-fs: dropping a client: %s\n", err.what());
				gone.push_back(c);
			}
		}
		for (auto &reply : server.TakeReplies()) {
			try {
				SendFrame(reply.first, reply.second);
			} catch (Error &err) {
				fprintf(stderr, "xnvme-fs: dropping a client: %s\n", err.what());
				if (std::find(gone.begin(), gone.end(), reply.first) == gone.end()) {
					gone.push_back(reply.first);
				}
			}
		}
		for (int c : gone) {
			server.Disconnect(c);
			close(c);
			clients.erase(std::find(clients.begin(), clients.end(), c));
		}
		if (fds[0].revents & POLLIN) {
			int c = accept4(lfd, nullptr, nullptr, SOCK_CLOEXEC);
			if (c >= 0) {
				server.Connect(c);
				clients.push_back(c);
			}
		}
	}
	for (int c : clients) {
		server.Disconnect(c);
		close(c);
	}
	server.CloseView();
	close(lfd);
	unlink(a.socket.c_str());
	for (int fd : locks) {
		close(fd);
	}
	fprintf(stderr, "xnvme-fs: stopped\n");
	return 0;
}

static int Mkfs(const Args &a) {
	auto locks = LockMembers(a.members);
	auto pool = Pool::Format(a.pool, a.members, a.access, a.extent_size, a.overwrite);
	for (auto &m : pool->Members()) {
		printf("%s\t%llu\t%llu\n", m.path.c_str(), (unsigned long long)m.size, (unsigned long long)m.extents);
	}
	for (int fd : locks) {
		close(fd);
	}
	return 0;
}

static int Info(const Args &a) {
	auto pool = Pool::Open(a.pool, a.members, a.access);
	auto table = pool->ReadTable();
	auto &sb = pool->Super();
	printf("pool\t%s\nlayout_version\t%u\nextent_size\t%llu\nslot_size\t%llu\ngeneration\t%llu\n", sb.pool_name.c_str(),
	       sb.layout_version, (unsigned long long)sb.extent_size, (unsigned long long)sb.slot_size,
	       (unsigned long long)table.generation);
	for (auto &e : table.entries) {
		printf("%s\t%s\t%llu\t%zu\n", e.kind == EntryKind::DIRECTORY ? "dir" : "file", e.name.c_str(),
		       (unsigned long long)e.size, e.extents.size());
	}
	return 0;
}

static bool Mounted(const std::string &dir) {
	char real[PATH_MAX];
	if (!realpath(dir.c_str(), real)) {
		return false;
	}
	FILE *f = fopen("/proc/self/mountinfo", "r");
	if (!f) {
		return false;
	}
	// Field 5 is the mount point, field 9, after the separator "-", the type: fuse.xnvme-fs
	char line[4096];
	bool found = false;
	while (!found && fgets(line, sizeof(line), f)) {
		char point[PATH_MAX], type[64];
		if (sscanf(line, "%*s %*s %*s %*s %4095s %*[^-]- %63s", point, type) == 2) {
			found = std::string(point) == real && std::string(type) == "fuse.xnvme-fs";
		}
	}
	fclose(f);
	return found;
}

//! Serves the pool on MEMBER in a daemon of its own, which the view's unmount stops, and returns once the view is
//! mounted, or fails with what stopped the daemon. The daemon reads the superblock, not this process, so nothing
//! of xNVMe's or HOMI's crosses the fork.
static int Mount(Args a) {
	if (Mounted(a.view)) {
		Fail(EBUSY, "xnvme fs: '%s' is already mounted", a.view.c_str());
	}
	int pipefd[2];
	if (pipe2(pipefd, O_CLOEXEC) != 0) {
		Fail(errno, "xnvme fs: pipe: %s", strerror(errno));
	}
	pid_t pid = fork();
	if (pid < 0) {
		Fail(errno, "xnvme fs: fork: %s", strerror(errno));
	}
	if (pid == 0) {
		close(pipefd[0]);
		setsid();
		int status = 0;
		try {
			dup2(pipefd[1], STDERR_FILENO);
			auto sb = Pool::Peek(a.members[0], a.access);
			a.pool = sb.pool_name;
			a.members = sb.members;
			DefaultSocket(a);
			// The daemon outlives mount.xnvme-fs, so it logs to a file, whose name the pipe carries first
			mkdir("/run/xnvme-fs", 0755);
			auto log = "/run/xnvme-fs/" + a.pool + ".log";
			fprintf(stderr, "log %s\n", log.c_str());
			int logfd = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
			int devnull = open("/dev/null", O_RDWR);
			dup2(devnull, STDIN_FILENO);
			dup2(devnull, STDOUT_FILENO);
			dup2(logfd >= 0 ? logfd : devnull, STDERR_FILENO);
			close(devnull);
			status = Serve(a);
		} catch (Error &err) {
			fprintf(stderr, "xnvme-fs: %s\n", err.what());
			status = 1;
		}
		_exit(status);
	}
	close(pipefd[1]);
	std::string log;
	int status = 0;
	while (true) {
		if (waitpid(pid, &status, WNOHANG) == pid) {
			char buf[512];
			ssize_t n;
			while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
				log.append(buf, size_t(n));
			}
			if (log.rfind("log ", 0) == 0) {
				auto path = log.substr(4, log.find('\n') - 4);
				log.erase(0, log.find('\n') + 1);
				if (FILE *f = fopen(path.c_str(), "r")) {
					while (fgets(buf, sizeof(buf), f)) {
						log += buf;
					}
					fclose(f);
				}
			}
			fprintf(stderr, "%s", log.c_str());
			Fail(EIO, "xnvme fs: the daemon for '%s' stopped before mounting the view", a.members[0].c_str());
		}
		if (Mounted(a.view)) {
			close(pipefd[0]);
			return 0;
		}
		usleep(20000);
	}
}

int main(int argc, char **argv) {
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--help")) {
			Usage(true);
		}
	}
	if (argc >= 2 && !strcmp(argv[1], "cp")) {
		return xnvme_fs_cp_main(argc - 1, argv + 1);
	}
	auto a = Parse(argc, argv);
	try {
		if (a.command == "mkfs") {
			return Mkfs(a);
		} else if (a.command == "mount") {
			return Mount(a);
		} else if (a.command == "serve") {
			return Serve(a);
		} else if (a.command == "info") {
			return Info(a);
		}
	} catch (Error &err) {
		fprintf(stderr, "xnvme-fs: %s\n", err.what());
		return 1;
	}
	Usage();
}
