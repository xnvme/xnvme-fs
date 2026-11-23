# SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
# SPDX-License-Identifier: BSD-3-Clause
"""
xnvme_fs_build: build xNVMe, xnvme fs and the adapters, on the host
===================================================================

Copies three trees from this machine to xnvme_fs.tree on the host, as they are checked out, untracked files
included: xNVMe from the checkout xnvme_fs.source.xnvme names, and xnvme fs and its adapters from
xnvme_fs.source.adapters, by default the checkout this script is in, with its DuckDB, extension-ci-tools and RocksDB
submodules. It records each checkout's commit, how many tracked files differ from it and a checksum of that
difference in artifacts/build.txt, and the two commits in COMMIT in the tree, which every part copies to its
artifacts.

On the host it builds xNVMe and installs it into the tree's prefix, which brings 'xnvme' and 'xnvme homi', then
builds xnvme fs against that xNVMe and installs it into the same prefix, which brings libxnvme-fs and xnvme-fs,
the extension that 'xnvme fs' runs. It then makes xnvme_fs.build.targets in the adapters' tree against that prefix: by
default DuckDB with the xnvme_fs extension, db_bench with the plugin, and db_bench as RocksDB's source builds it,
with no plugin. RocksDB needs gflags; a host without it gets gflags v2.2.2 built into the tree, as a static library.
With xnvme_fs.lab.duckdb_url it also fetches DuckDB's official CLI to stock/duckdb in the tree, as DuckDB ships it,
and records its sha256 and version. The stock programs are what the runs on XFS measure, so the step fails when the
stock db_bench links libxnvme-fs.

A tree the host already holds from the same commits, differences and targets is kept, and nothing is built. The
builds use no ccache unless xnvme_fs.build.ccache names its directory: ccache's default is in the home directory,
which on a lab host is its network root. xnvme_fs.build.xnvme_options adds meson options to xNVMe's build, such as
disabling a library this machine has and the machine that runs the build does not; they are part of the build's
record, so a tree built without them is rebuilt.

Retargetable: False
-------------------
"""
import errno
import hashlib
import logging as log
import tempfile
from pathlib import Path

# What each build needs: the checkout it comes from, the directory in it, and what stays behind. The subprojects
# of xNVMe and of xnvme fs are fetched on the host at the revisions their wraps pin
TREES = {
    "xnvme": ("xnvme", ".", ["./builddir", "./.git", "./subprojects/upcie", "./subprojects/spdk"]),
    "fs": ("adapters", ".", ["./builddir", "./.git", "./subprojects/ufuse", "./adapters", "./cijoe"]),
    "adapters": ("adapters", "adapters", ["./build", "./duckdb/.git", "./extension-ci-tools/.git",
                                         "./third_party/rocksdb/.git", "./duckdb_unittest_tempdir", "./docs/_build",
                                         "./docs/.venv"]),
}
REPOS = ("xnvme", "adapters")
SUBMODULES = ["adapters/duckdb", "adapters/extension-ci-tools", "adapters/third_party/rocksdb"]
TARGETS = "release rocksdb-release rocksdb-stock"
GFLAGS = "https://github.com/gflags/gflags/archive/refs/tags/v2.2.2.tar.gz"
# No GPU backend, whatever toolkits the host has: the file system's members are NVMe controllers
MESON = {
    "xnvme": ("-Dwith-spdk=disabled -Dwith-libvfn=disabled -Dwith-cuda=disabled -Dwith-hip=disabled"
              " -Dexamples=false -Dtests=false -Dsystemd=disabled"),
    "fs": "-Dtests=false -Dsystemd=disabled -Ddmabuf=enabled",
}


def paths(cijoe):
    """Where the build is on the host: the tree, the install prefix of xNVMe and xnvme fs in it, and the adapters' tree, which the
    parts run from"""
    tree = str(cijoe.getconf("xnvme_fs.tree", "") or "/dev/shm/xnvme-fs-lab")
    return {"tree": tree, "prefix": f"{tree}/prefix", "adapters": f"{tree}/adapters"}


def built(cijoe):
    """0 where the host holds a tree that xnvme_fs_build finished, else an errno. cijoe runs the steps after one
    that failed, so a step that wipes, stripes or formats the drives, or measures on them, asks this first. BUILD
    is written last, and main() removes it when the step fails"""
    tree = paths(cijoe)["tree"]
    err, _ = cijoe.run(f"test -s {tree}/BUILD && test -s {tree}/build.txt")
    if err:
        log.error(f"{tree} holds no finished build: xnvme_fs_build failed, or did not run")
        return errno.ENOENT
    return 0


def source(cijoe, name):
    """The checkout xnvme_fs.source.NAME names; for the adapters, by default, the one this script is in"""
    repo = str(cijoe.getconf(f"xnvme_fs.source.{name}", "") or "")
    if repo:
        return Path(repo).expanduser()
    return Path(__file__).resolve().parents[2] if name == "adapters" else None


def failed(logfile):
    """The shell words printing the errors of a build's log and failing"""
    return f"{{ grep -E 'error|Error|ERROR' -A3 {logfile} | head -40; exit 1; }}"


def install_xnvme_fs(cijoe, p, xnvme, fs, env):
    """Builds, on the host, xNVMe from the source tree xnvme and then xnvme fs from the source tree fs, and
    installs both into the prefix: 'xnvme' and its program xnvme-homi from the first, and from the second
    libxnvme-fs with its pkg-config file and xnvme-fs, in the directory xNVMe's front-end runs its programs
    from. env is exported to the builds. xnvme fs finds xNVMe by pkg-config in the prefix, so a build of xNVMe
    that fails stops before xnvme fs is configured. Returns the error"""
    tree, prefix = p["tree"], p["prefix"]
    options = dict(MESON)
    options["xnvme"] += " " + str(cijoe.getconf("xnvme_fs.build.xnvme_options", "") or "").strip()
    # xNVMe installs its bash completion to the completionsdir of the host's bash-completion.pc, outside the
    # prefix; a bash-completion.pc first on the path keeps it in the prefix
    stub, completions = f"{tree}/pkgconfig-stub", f"{prefix}/share/bash-completion/completions"
    err, _ = cijoe.run(
        f"mkdir -p {stub} && printf 'completionsdir={completions}\\nName: bash-completion\\nDescription: stub\\n"
        f"Version: 2.11\\n' > {stub}/bash-completion.pc"
    )
    if err:
        return err
    for name, source, exports, installed in (
        ("xnvme", xnvme, f"{env} PKG_CONFIG_PATH={stub}",
         f"test -x {prefix}/bin/xnvme && test -n \"$(find {prefix} -name xnvme.pc)\" && test -f {completions}/xnvme"),
        ("fs", fs, f"{env} PKG_CONFIG_PATH=$(dirname $(find {prefix} -name xnvme.pc))",
         f"test -x \"$({prefix}/bin/xnvme --exec-path)/xnvme-fs\" && {prefix}/bin/xnvme fs --help > /dev/null"
         f" && PKG_CONFIG_PATH=$(dirname $(find {prefix} -name xnvme-fs.pc)) pkg-config --exists xnvme-fs"),
    ):
        build, logfile = f"{tree}/build-{name}", f"{tree}/build-{name}.log"
        err, _ = cijoe.run(
            f"export {exports}; {{ meson setup {build} {source} --prefix {prefix} {options[name]} > {logfile} 2>&1"
            f" && meson compile -C {build} >> {logfile} 2>&1 && meson install -C {build} >> {logfile} 2>&1"
            f" || {failed(logfile)}; }} && rm -rf {build} && {installed}"
        )
        if err:
            log.error(f"{name} did not build and install into {prefix}; see {logfile} on the host")
            return err
    return 0


def main(args, cijoe):
    # With xnvme_fs.lab.marker, only when the marker names xnvme fs: cijoe runs a step after one that failed
    marker = cijoe.getconf("xnvme_fs.lab.marker", "")
    if marker:
        err, _ = cijoe.run(f"grep -q '^xnvme-fs:' {marker}")
        if err:
            log.error(f"{marker} is not xnvme fs's; the host was not taken")
            return errno.EBUSY
    err = build(cijoe)
    if err:
        # Whatever failed, and however early: a tree of an earlier build is not this run's, see built()
        cijoe.run(f"rm -f {paths(cijoe)['tree']}/BUILD")
    return err


def build(cijoe):
    """Copies the trees to the host and builds them there, unless the host holds the same build. Returns the
    error"""
    p = paths(cijoe)
    tree, prefix, adapters = p["tree"], p["prefix"], p["adapters"]
    targets = str(cijoe.getconf("xnvme_fs.build.targets", "") or TARGETS)
    url = str(cijoe.getconf("xnvme_fs.lab.duckdb_url", "") or "").strip()
    artifacts = Path(cijoe.output_path) / "artifacts"
    artifacts.mkdir(parents=True, exist_ok=True)

    record, commits, repos = [], [], {}
    for name in REPOS:
        repo = source(cijoe, name)
        needed = SUBMODULES if name == "adapters" else []
        if repo is None or not (repo / ".git").exists() or not all((repo / s / ".git").exists() for s in needed):
            log.error(f"xnvme_fs.source.{name}({repo}) is not a git checkout, or lacks one of {needed}:"
                      f" git submodule update --init {' '.join(needed)}")
            return errno.ENOENT
        err, state = cijoe.run_local(
            f"git -C {repo} rev-parse HEAD && git -C {repo} status --porcelain --untracked-files=no | wc -l"
            f" && git -C {repo} diff HEAD | sha256sum | cut -c1-12 && git -C {repo} log -1 --format=%s"
        )
        if err:
            return err
        commit, dirty, diff, subject = state.output().strip().splitlines()[-4:]
        record.append(f"{name}: {commit} ({dirty.strip()} tracked files changed, diff {diff}) {subject}")
        commits.append(f"{name} {commit}")
        repos[name] = repo
    # The submodules come along without git, so DuckDB's version and commit are given to its build rather than found
    # by it: the tag alone when the commit is the tag's, as DuckDB names a release
    described = {}
    for sub in ("duckdb", "third_party/rocksdb"):
        at = f"{repos['adapters']}/adapters/{sub}"
        err, state = cijoe.run_local(f"git -C {at} describe --tags --long --abbrev=10 && git -C {at} rev-parse HEAD")
        if err or not state.output().strip():
            log.error(f"git does not describe {at}; a shallow clone needs its tag: git -C {at} fetch --depth 1 origin"
                      " tag <tag>")
            return err or errno.ENOENT
        described[sub], commit = state.output().strip().splitlines()[-2:]
        record.append(f"  {sub.split('/')[-1]}: {described[sub]}")
        if sub == "duckdb":
            tag, since, _ = described[sub].rsplit("-", 2)
            duckdb = f"OVERRIDE_GIT_DESCRIBE={tag if since == '0' else described[sub]} DUCKDB_COMMIT={commit}"
    record.append(f"targets: {targets}")
    xnvme_options = str(cijoe.getconf("xnvme_fs.build.xnvme_options", "") or "").strip()
    if xnvme_options:
        record.append(f"xnvme options: {xnvme_options}")
    if url:
        record.append(f"duckdb-stock: {url}")
    key = "\n".join(record) + "\n"
    digest = hashlib.sha256(key.encode()).hexdigest()

    err, _ = cijoe.run(f"test \"$(sha256sum < {tree}/BUILD | cut -d' ' -f1)\" = {digest} && test -s {tree}/build.txt")
    if not err:
        _, state = cijoe.run(f"cat {tree}/build.txt")
        (artifacts / "build.txt").write_text(state.output())
        return 0

    err, _ = cijoe.run(f"rm -rf {tree} && mkdir -p {tree}/src")
    if err:
        return err
    with tempfile.TemporaryDirectory() as tmp:
        for name, (repo, sub, excludes) in TREES.items():
            tarball = Path(tmp) / f"{name}.tar.gz"
            ex = " ".join(f"--exclude='{e}'" for e in excludes)
            err, _ = cijoe.run_local(f"tar -C {repos[repo] / sub} {ex} -czf {tarball} .")
            if err:
                return err
            if not cijoe.put(str(tarball), f"{tree}/{name}.tar.gz"):
                log.error(f"could not copy {name} to the host")
                return errno.EIO
            tarball.unlink()
            to = adapters if name == "adapters" else f"{tree}/src/{name}"
            err, _ = cijoe.run(f"mkdir -p {to} && tar -C {to} -xzf {tree}/{name}.tar.gz && rm {tree}/{name}.tar.gz")
            if err:
                return err

    # meson takes ccache when it finds it, and DuckDB and RocksDB when the launchers name it
    ccache = str(cijoe.getconf("xnvme_fs.build.ccache", "") or "")
    cc = (f"CCACHE_DIR={ccache} CMAKE_C_COMPILER_LAUNCHER=ccache CMAKE_CXX_COMPILER_LAUNCHER=ccache" if ccache
          else "CCACHE_DISABLE=1")
    pc = f"PKG_CONFIG_PATH=$(dirname $(find {prefix} -name xnvme-fs.pc))"
    err = install_xnvme_fs(cijoe, p, f"{tree}/src/xnvme", f"{tree}/src/fs", cc)
    if err:
        return err

    exports = f"export {cc} GEN=ninja {pc}"
    if "rocksdb" in targets:
        err, _ = cijoe.run("test -e /usr/include/gflags/gflags.h")
        if err:
            err, state = cijoe.run(
                f"mkdir -p {tree}/src/gflags && cd {tree}/src && curl -sSfL -o gflags.tar.gz {GFLAGS}"
                f" && sha256sum gflags.tar.gz && tar -C gflags --strip-components=1 -xzf gflags.tar.gz"
                f" && rm gflags.tar.gz && {{ cmake -S gflags -B gflags/build -G Ninja -DCMAKE_BUILD_TYPE=Release"
                f" -DBUILD_SHARED_LIBS=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DREGISTER_INSTALL_PREFIX=OFF"
                f" -DCMAKE_INSTALL_PREFIX={tree}/gflags > {tree}/build-gflags.log 2>&1"
                f" && cmake --build gflags/build --target install >> {tree}/build-gflags.log 2>&1"
                f" || {failed(f'{tree}/build-gflags.log')}; }} && rm -rf gflags"
            )
            if err:
                log.error("the host has no gflags, and it did not build")
                return err
            record.append(f"gflags: {GFLAGS}, static, sha256 {state.output().split()[0]}")
            exports += f" CMAKE_PREFIX_PATH={tree}/gflags"

    for target in targets.split():
        logfile = f"{tree}/build-{target}.log"
        # The object files go once a target is linked: a tree is built whole or not at all, and a host's tmpfs or
        # network root has little room for them
        err, _ = cijoe.run(
            f"cd {adapters} && {exports} {duckdb} && {{ make {target}"
            f" > {logfile} 2>&1 || {failed(logfile)}; }} && find build -name '*.o' -delete"
        )
        if err:
            log.error(f"make {target} failed")
            return err

    checks = [f"echo libxnvme-fs: $({pc} pkg-config --modversion xnvme-fs)"]
    if "release" in targets:
        checks.append(
            f"{adapters}/build/release/duckdb -version && {adapters}/build/release/duckdb -init /dev/null -noheader"
            f" -list -c \"SELECT string_agg(extension_name, ' ' ORDER BY extension_name) FROM duckdb_extensions()"
            f" WHERE install_mode = 'STATICALLY_LINKED'\""
        )
    if "rocksdb-stock" in targets:
        # Stock: nothing of xnvme fs is linked in
        checks.append(
            f"! ldd {adapters}/build/rocksdb-stock/db_bench | grep xnvme"
            f" && echo \"db_bench-stock: no libxnvme among $(ldd {adapters}/build/rocksdb-stock/db_bench | wc -l)"
            f" libraries\""
        )
    if url:
        checks.append(
            f"mkdir -p {tree}/stock && cd {tree}/stock && curl -sSfL -o duckdb.tar.gz {url} && sha256sum duckdb.tar.gz"
            " && tar -xzf duckdb.tar.gz && rm duckdb.tar.gz && ./duckdb -version"
        )
    for cmd in checks:
        err, state = cijoe.run(cmd)
        if err:
            log.error(f"failed: {cmd}")
            return err
        record += [f"  {line}" for line in state.output().strip().splitlines()]

    text = "\n".join(record) + "\n"
    (artifacts / "build.txt").write_text(text)
    with tempfile.TemporaryDirectory() as tmp:
        for name, content in (("build.txt", text), ("COMMIT", "\n".join(commits) + "\n"), ("BUILD", key)):
            (Path(tmp) / name).write_text(content)
            if not cijoe.put(str(Path(tmp) / name), f"{tree}/{name}"):
                log.error(f"could not write {tree}/{name} on the host")
                return errno.EIO
    return 0
