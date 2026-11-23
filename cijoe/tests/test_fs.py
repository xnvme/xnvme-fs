"""
xnvme fs on controllers a homi server holds, which the tests overwrite

The member's own reads and writes are tested on the controller directly; the
rest on a pool made with 'xnvme fs mkfs' and served by 'xnvme fs serve' for
each test.

A test of a pool on one controller takes the first one labelled "pcie". A test
that needs several, or that partitions a controller, takes those labelled "fs"
as well, in the order of the configuration, and skips where there are too few.
The tests of GPT partitions and md RAID 0 sets hand the controllers to the
kernel's driver to make them, with sfdisk and mdadm as an administrator would,
and hand them back to the homi server to read them; they remove the partition
tables and md superblocks when done.
"""

import random
import re

import pytest

from .conftest import (
    HomiServer,
    XnvmeDriver,
    cijoe_config_get_all_devices,
    get_homi_id,
    xnvme_parametrize,
)

POOL = "ci"
WORK = "/tmp/xnvme-fs-test"
VIEW = f"{WORK}/view"

# Where the helpers 'mkfs.xnvme-fs' and 'mount.xnvme-fs' serve a pool
RUN = "/run/xnvme-fs"

POKE = "xnvme test fs-poke"

# GPT partition types: the one xnvme fs takes as a member, a Linux filesystem, and Linux RAID
GPT_FS = "a04faa88-135a-4457-b8f6-db8bbaf344b8"
GPT_LINUX = "0fc63daf-8483-4772-8e79-3d69d8477de4"
GPT_RAID = "a19d880f-05fc-4d3b-a006-743f0f84911e"

MIB = 1024 * 1024

# What the kernel writes through an md array, and xnvme fs through the set: 8 MiB
# starting at a block that is not a whole chunk, so it spans the drives unevenly
PATTERN_NBYTES = 8 * MIB
PATTERN_BLOCK = 1283


def _built(cijoe):
    """Fail unless 'xnvme' runs xnvme-fs, which this project installs as its extension"""

    err, _ = cijoe.run("xnvme fs --help")
    assert not err, "'xnvme fs' does not run; xnvme-fs is not on xnvme's exec path"


def _served(cijoe, be_opts):
    """Skip unless a homi server holds the controller; return its homi id"""

    _built(cijoe)

    homi_id = get_homi_id()
    if not homi_id:
        pytest.skip(reason="Requires a homi server; pass --homi-id")
    if be_opts["be"] != "upcie":
        pytest.skip(reason="xnvme fs opens its members with the upcie backend")

    return homi_id


def _require(cijoe, *tools):
    """Skip unless the target has each of the tools"""

    for tool in tools:
        err, _ = cijoe.run(f"command -v {tool}")
        if err:
            pytest.skip(reason=f"Requires {tool}, which is not installed")


def _require_fuse(cijoe):
    """Skip unless the target can mount the view"""

    err, _ = cijoe.run("[ -c /dev/fuse ] || modprobe fuse; [ -c /dev/fuse ]")
    if err:
        pytest.skip(reason="Requires /dev/fuse for the view")


def _controllers(count):
    """The first 'count' controllers the tests may partition and combine; skip where there are fewer"""

    bdfs = [d["uri"] for d in cijoe_config_get_all_devices(["pcie", "fs"])]
    if len(bdfs) < count:
        pytest.skip(reason=f"Requires {count} devices labelled: ['pcie', 'fs']")

    return bdfs[:count]


def _word(cijoe, cmd):
    """The last word a command prints"""

    err, state = cijoe.run(cmd)
    assert not err, f"failed: {cmd}"

    return state.output().split()[-1]


def _fails(cijoe, cmd, pattern):
    """The command fails, and says 'pattern'"""

    err, state = cijoe.run(f"{cmd} 2>&1")
    assert err, f"expected failure from: {cmd}"
    assert pattern in state.output(), f"expected '{pattern}' from: {cmd}"


def _workdir(cijoe):
    """An empty working directory, and the mkfs and mount helpers in it under the names they are called by"""

    err, _ = cijoe.run(
        f"rm -rf {WORK} && mkdir -p {VIEW}"
        f" && ln -s $(xnvme --exec-path)/xnvme-fs {WORK}/mkfs.xnvme-fs"
        f" && ln -s $(xnvme --exec-path)/xnvme-fs {WORK}/mount.xnvme-fs"
    )
    assert not err


def _access(members, homi_id):
    return " ".join(f"--member {m}" for m in members) + f" --homi-id {homi_id}"


def _start(cijoe, pool, access, opts=""):
    """Serve the pool from the background"""

    # A socket a killed server left behind would pass for the one this server opens
    err, _ = cijoe.run(
        f"rm -f {WORK}/{pool}.sock;"
        f" nohup xnvme fs serve --pool {pool} {access} --socket {WORK}/{pool}.sock {opts}"
        f" >> {WORK}/serve.log 2>&1 < /dev/null & echo $! > {WORK}/serve.pid"
    )
    assert not err

    # The front end execs xnvme-fs, so the pid is the server's
    err, _ = cijoe.run(
        f"for i in $(seq 1 100); do [ -S {WORK}/{pool}.sock ] && exit 0;"
        f" kill -0 $(cat {WORK}/serve.pid) || break; sleep 0.1; done;"
        f" cat {WORK}/serve.log; exit 1"
    )
    assert not err, "xnvme fs serve did not open its socket"


def _serve(cijoe, device, homi_id, opts=""):
    """Format a pool on the device and serve it from the background; return the clients' environment"""

    access = _access([device["uri"]], homi_id)

    _workdir(cijoe)

    err, _ = cijoe.run(
        f"xnvme fs mkfs --pool {POOL} {access} --extent-size 1M --overwrite"
    )
    assert not err, "xnvme fs mkfs failed"

    _start(cijoe, POOL, access, opts)

    return f"XNVME_FS_SOCKET_DIR={WORK} XNVME_FS_HOMI_ID={homi_id}"


def _stop(cijoe):
    """Stop the server; fail if it died during the test or does not stop on SIGTERM"""

    err, _ = cijoe.run(f"kill $(cat {WORK}/serve.pid)")
    assert not err, "xnvme fs serve died during the test"

    err, _ = cijoe.run(
        f"for i in $(seq 1 100); do kill -0 $(cat {WORK}/serve.pid) 2>/dev/null || exit 0;"
        f" sleep 0.1; done; exit 1"
    )
    assert not err, "xnvme fs serve did not stop on SIGTERM"

    cijoe.run(f"cat {WORK}/serve.log")


def _kill(cijoe, pidfile):
    """Kill the process with SIGKILL, if it runs, and wait until it is gone"""

    cijoe.run(
        f"kill -9 $(cat {pidfile}) 2>/dev/null;"
        f" for i in $(seq 1 100); do kill -0 $(cat {pidfile}) 2>/dev/null || exit 0;"
        f" sleep 0.1; done; exit 1"
    )


def _to_kernel(cijoe, bdfs):
    """Hand the controllers to the kernel's driver; return the block device of each one's first namespace"""

    XnvmeDriver.kernel_attach(cijoe)

    # An md set udev assembled from what an earlier run left holds its drives
    cijoe.run("udevadm settle; command -v mdadm && mdadm --stop --scan; true")

    devs = []
    for bdf in bdfs:
        name = _word(
            cijoe,
            f"for i in $(seq 1 100); do ls /sys/bus/pci/devices/{bdf}/nvme/*/ 2>/dev/null"
            " | grep -m1 -E '^nvme[0-9]+(c[0-9]+)?n1$' | sed -E 's/c[0-9]+n/n/' | grep . && exit 0;"
            " sleep 0.1; done; exit 1",
        )
        devs.append(f"/dev/{name}")

    return devs


def _to_homi(cijoe, be_opts):
    """Hand the controllers back to a homi server, as the 'device' fixture started it"""

    cijoe.run("udevadm settle; command -v mdadm && mdadm --stop --scan; true")
    HomiServer.start(cijoe, HomiServer.serve_be(be_opts["be"]), be_opts.get("label"))


def _wipe(cijoe, devs):
    """Remove the signatures on each drive's partitions, and then the drive's own"""

    for dev in devs:
        cijoe.run(
            f"for d in {dev}p* {dev}; do [ -b $d ] && wipefs -q -a $d; done;"
            f" blockdev --rereadpt {dev}; true"
        )


def _unpartition(cijoe, bdfs):
    """Remove the md superblocks and partition tables the test made, leaving the controllers with the kernel"""

    _wipe(cijoe, _to_kernel(cijoe, bdfs))


def _partition(cijoe, dev, parts):
    """Write a GPT with a partition for each (MiB, type) of 'parts'; return each one's (offset, nbytes) on the drive"""

    script = "label: gpt\\n" + "".join(
        f"size={mib}MiB, type={kind}\\n" for mib, kind in parts
    )
    _wipe(cijoe, [dev])
    err, _ = cijoe.run(
        f"printf '{script}' | sfdisk -q --wipe always --wipe-partitions always {dev}"
        f" && for i in $(seq 1 100); do [ -b {dev}p{len(parts)} ] && exit 0; sleep 0.1; done; exit 1"
    )
    assert not err, f"could not partition {dev}"

    name = dev[len("/dev/") :]
    ranges = []
    for number in range(1, len(parts) + 1):
        sysfs = f"/sys/class/block/{name}p{number}"
        ranges.append(
            (
                512 * int(_word(cijoe, f"cat {sysfs}/start")),
                512 * int(_word(cijoe, f"cat {sysfs}/size")),
            )
        )

    return ranges


def _md_create(cijoe, array, devs, level=0, opts=""):
    """Make an md set of the devices, in their order, and leave the array running"""

    err, _ = cijoe.run(
        f"wipefs -q -a {' '.join(devs)}; yes | mdadm --create {array} --run --metadata=1.2 --level={level}"
        f" {opts} --raid-devices={len(devs)} {' '.join(devs)} && mdadm --detail {array}"
    )
    assert not err, f"mdadm could not create {array}"


def _md_pattern(cijoe, array, name):
    """Write a pattern through the running array, and stop it; return the array's size in bytes"""

    nbytes = 512 * int(
        _word(cijoe, f"cat /sys/class/block/{array[len('/dev/'):]}/size")
    )
    err, _ = cijoe.run(
        f"head -c {PATTERN_NBYTES} /dev/urandom > {WORK}/{name}"
        f" && dd if={WORK}/{name} of={array} bs=4096 seek={PATTERN_BLOCK} oflag=direct conv=fsync status=none"
        f" && mdadm --stop {array}"
    )
    assert not err, f"could not write through {array}"

    return nbytes


def _md_holds(cijoe, array, devs, offset, name):
    """Assemble the set and check that the array holds the file at 'offset'; stop it"""

    err, _ = cijoe.run(
        f"mdadm --assemble {array} {' '.join(devs)}"
        f" && dd if={array} bs=4096 skip={offset // 4096} count={PATTERN_NBYTES // 4096} status=none"
        f" | cmp - {WORK}/{name}"
    )
    cijoe.run(f"mdadm --stop {array}")
    assert not err, f"{array} does not hold what xnvme fs wrote to the set"


def _poke_holds(cijoe, poke, offset, name):
    """Check that the member 'poke' reads holds the file at 'offset'"""

    err, _ = cijoe.run(f"{poke} {offset} --read {PATTERN_NBYTES} | cmp - {WORK}/{name}")
    assert not err, f"the member does not hold {name} at {offset}"


def _poke_write(cijoe, poke, offset, name):
    """Write a new pattern to the member 'poke' writes at 'offset', and read it back"""

    err, _ = cijoe.run(
        f"head -c {PATTERN_NBYTES} /dev/urandom > {WORK}/{name}"
        f" && {poke} {offset} --write {PATTERN_NBYTES} < {WORK}/{name}"
    )
    assert not err, "could not write to the member"

    _poke_holds(cijoe, poke, offset, name)


def _mount(cijoe, env, member):
    err, _ = cijoe.run(
        f"{env} {WORK}/mount.xnvme-fs {member} {VIEW} && mountpoint -q {VIEW}"
    )
    assert not err, "mount.xnvme-fs returned without the view mounted"


def _unmount(cijoe, label):
    """Unmount the view and wait for its server to remove the socket, as it does on stopping"""

    err, _ = cijoe.run(
        f"umount {VIEW} && for i in $(seq 1 100); do [ -S {RUN}/{label}.sock ] || exit 0; sleep 0.1; done;"
        f" cat {RUN}/{label}.log; exit 1"
    )
    assert not err, f"xnvme fs still serves '{label}' after the view was unmounted"


def _mounted_pool_keeps_a_file(cijoe, env, member, label):
    """Format the member, mount it, write a file, and read it back after unmounting and mounting again"""

    err, _ = cijoe.run(f"{env} {WORK}/mkfs.xnvme-fs -L {label} -f {member}")
    assert not err, "mkfs.xnvme-fs failed"

    _mount(cijoe, env, member)
    # Not a multiple of the block size, and larger than an extent
    err, _ = cijoe.run(
        f"head -c 5242883 /dev/urandom > {WORK}/in.bin"
        f" && {env} xnvme fs cp {label} in {WORK}/in.bin t.db && cmp {VIEW}/t.db {WORK}/in.bin"
    )
    assert not err, "the view does not show the file written to the pool"
    _fails(cijoe, f"{env} {WORK}/mount.xnvme-fs {member} {VIEW}", "already mounted")
    _unmount(cijoe, label)

    _mount(cijoe, env, member)
    err, _ = cijoe.run(
        f"{env} xnvme fs cp {label} out t.db {WORK}/out.bin && cmp {WORK}/in.bin {WORK}/out.bin"
    )
    assert not err, "the pool lost the file over an unmount"
    _unmount(cijoe, label)


@xnvme_parametrize(labels=["pcie"], opts=["be"])
def test_member(cijoe, device, be_opts, cli_args):
    """A member's byte-addressed reads and writes against an in-memory copy"""

    homi_id = _served(cijoe, be_opts)

    err, _ = cijoe.run(f"xnvme test fs-member {device['uri']} {homi_id}")
    assert not err


@xnvme_parametrize(labels=["pcie"], opts=["be"])
def test_capi(cijoe, device, be_opts, cli_args):
    """The C interface, with appends written through and with appends held until a sync"""

    homi_id = _served(cijoe, be_opts)
    env = _serve(cijoe, device, homi_id)

    try:
        err, _ = cijoe.run(f"{env} xnvme test fs-capi {POOL}")
        assert not err, "the C interface failed with appends written through"

        err, _ = cijoe.run(f"{env} XNVME_FS_APPEND_BUFFER=1 xnvme test fs-capi {POOL}")
        assert not err, "the C interface failed with appends held until a sync"
    finally:
        _stop(cijoe)


@xnvme_parametrize(labels=["pcie"], opts=["be"])
def test_index(cijoe, device, be_opts, cli_args):
    """The index a reader outside the engines takes of the pool, and its leases"""

    homi_id = _served(cijoe, be_opts)
    env = _serve(cijoe, device, homi_id)

    try:
        err, _ = cijoe.run(f"{env} xnvme test fs-index {POOL}")
        assert not err
    finally:
        _stop(cijoe)


@xnvme_parametrize(labels=["pcie"], opts=["be"])
def test_cp(cijoe, device, be_opts, cli_args):
    """'xnvme fs cp' copies a file into the pool and back out, byte for byte"""

    homi_id = _served(cijoe, be_opts)
    env = _serve(cijoe, device, homi_id)

    # Not a multiple of the block size, and larger than one command moves
    try:
        err, _ = cijoe.run(
            f"head -c 5242883 /dev/urandom > {WORK}/in.bin"
            f" && {env} xnvme fs cp {POOL} in {WORK}/in.bin copied.bin"
            f" && {env} xnvme fs cp {POOL} out copied.bin {WORK}/out.bin"
            f" && cmp {WORK}/in.bin {WORK}/out.bin"
        )
        assert not err, "the file copied out differs from the one copied in"

        # A failure says which errno and what the library made of it
        _fails(cijoe, f"{env} xnvme fs cp {POOL} out nosuch.bin /dev/null", "No such")
        _fails(
            cijoe, f"{env} xnvme fs cp {POOL} out nosuch.bin /dev/null", "'nosuch.bin'"
        )

        # Without XNVME_FS_SOCKET_DIR, the pool's socket is where the unit puts it
        _fails(
            cijoe,
            f"XNVME_FS_HOMI_ID={homi_id} xnvme fs cp nosuch in {WORK}/in.bin copied.bin",
            f"{RUN}/nosuch.sock",
        )
    finally:
        _stop(cijoe)


@xnvme_parametrize(labels=["pcie", "fs"], opts=["be"])
def test_cli(cijoe, device, be_opts, cli_args):
    """What 'xnvme fs' accepts and refuses: formatting, damaged superblocks and tables, one server per pool"""

    homi_id = _served(cijoe, be_opts)
    m0, m1 = _controllers(2)
    _workdir(cijoe)

    def fs(command, pool, members, opts=""):
        return f"xnvme fs {command} --pool {pool} {_access(members, homi_id)} {opts}"

    def mkfs(pool, members):
        err, _ = cijoe.run(fs("mkfs", pool, members, "--extent-size 1M --overwrite"))
        assert not err, "xnvme fs mkfs failed"

    def poke(offset, hexbytes):
        err, _ = cijoe.run(f"{POKE} {m0} {homi_id} {offset} {hexbytes}")
        assert not err

    # A member already in a pool is refused unless asked to overwrite
    mkfs("p", [m0])
    _fails(
        cijoe,
        fs("mkfs", "p", [m0], "--extent-size 1M"),
        "already holds a pool superblock",
    )
    mkfs("p", [m0])
    err, state = cijoe.run(fs("info", "p", [m0]))
    assert not err
    assert re.search(
        r"^generation\t1$", state.output(), re.M
    ), "a fresh pool is not at generation 1"
    _fails(cijoe, fs("info", "q", [m0]), "belongs to pool 'p', not 'q'")

    # Asked for, the usage goes to stdout and is no failure; a command line not understood is one
    err, _ = cijoe.run("xnvme fs --help 2> /dev/null | grep -q '^usage: xnvme fs mkfs'")
    assert not err, "xnvme fs --help did not print the usage to stdout"
    err, _ = cijoe.run("xnvme fs --help > /dev/null")
    assert not err, "xnvme fs --help failed"
    _fails(cijoe, "xnvme fs info --nosuch x", "usage: xnvme fs mkfs")

    # The HOMI id comes from XNVME_FS_HOMI_ID where --homi-id is not given
    err, _ = cijoe.run(
        f"XNVME_FS_HOMI_ID={homi_id} xnvme fs info --pool p --member {m0}"
    )
    assert not err, "xnvme fs info did not take the HOMI id from XNVME_FS_HOMI_ID"
    _fails(
        cijoe,
        f"env -u XNVME_FS_HOMI_ID xnvme fs info --pool p --member {m0}",
        "set the HOMI id",
    )
    _fails(
        cijoe, fs("mkfs", "'bad name'", [m1], "--overwrite"), "may hold only letters"
    )
    _fails(cijoe, fs("mkfs", "p", [m1], "--extent-size 3M --overwrite"), "power of two")
    _fails(
        cijoe, fs("mkfs", "p", [m1], "--extent-size 1T --overwrite"), "holds no extent"
    )

    # The member list is part of the pool: a subset or a reordering is refused
    mkfs("pair", [m0, m1])
    err, _ = cijoe.run(fs("info", "pair", [m0, m1]))
    assert not err
    _fails(cijoe, fs("info", "pair", [m1, m0]), "lists members")
    _fails(cijoe, fs("info", "pair", [m0]), "lists members")

    # Superblocks that are not this layout's: the magic at offset 0, the layout version at 8, a byte the checksum covers
    mkfs("p", [m0])
    poke(0, "4e4e4e4e4e4e4e4e")
    _fails(cijoe, fs("info", "p", [m0]), "bad magic")
    mkfs("p", [m0])
    poke(8, "03000000")
    _fails(cijoe, fs("info", "p", [m0]), "layout version 3")
    mkfs("p", [m0])
    poke(100, "ff")
    _fails(cijoe, fs("info", "p", [m0]), "checksum mismatch")

    # A fresh pool's table is in slot 1; with it torn and slot 0 empty, the pool holds no table
    mkfs("torn", [m0])
    err, state = cijoe.run(fs("info", "torn", [m0]))
    assert not err
    slot = int(re.search(r"^slot_size\t(\d+)$", state.output(), re.M).group(1))
    poke(4096 + slot + 33, "58")
    _fails(cijoe, fs("info", "torn", [m0]), "holds no valid extent map table")

    # One server per controller, one server per socket
    mkfs("p", [m0])
    mkfs("q", [m1])
    _start(cijoe, "p", _access([m0], homi_id))
    try:
        socket = f"--socket {WORK}/p2.sock"
        _fails(
            cijoe, fs("serve", "p", [m0], socket), "in use by another 'xnvme fs serve'"
        )
        _fails(
            cijoe,
            fs("mkfs", "p", [m0], "--overwrite"),
            "in use by another 'xnvme fs serve'",
        )
        _fails(
            cijoe, fs("serve", "q", [m1], f"--socket {WORK}/p.sock"), "already served"
        )

        # A client that asks the server for another pool than the one it serves
        _fails(
            cijoe,
            f"ln -sf p.sock {WORK}/q.sock && XNVME_FS_SOCKET_DIR={WORK}"
            f" XNVME_FS_HOMI_ID={homi_id} xnvme fs cp q out none /dev/null",
            "this server serves pool 'p', not 'q'",
        )
    finally:
        _stop(cijoe)


@xnvme_parametrize(labels=["pcie"], opts=["be"])
def test_hold(cijoe, device, be_opts, cli_args):
    """One client per file: a second cannot open the file a first holds, and can once the first is killed"""

    homi_id = _served(cijoe, be_opts)
    env = _serve(cijoe, device, homi_id)
    hold = f"{env} xnvme test fs-hold {POOL} held.txt"

    try:
        # The front end execs the test, so the pid is the holder's
        err, _ = cijoe.run(
            f"nohup env {hold} first > {WORK}/hold.out 2>&1 < /dev/null & echo $! > {WORK}/hold.pid;"
            f" for i in $(seq 1 100); do grep -qx held {WORK}/hold.out && exit 0; sleep 0.1; done;"
            f" cat {WORK}/hold.out; exit 1"
        )
        assert not err, "the first client did not open the file"

        _fails(cijoe, f"{hold} second", "is open in another client")

        _kill(cijoe, f"{WORK}/hold.pid")
        err, _ = cijoe.run(
            f'{env} xnvme fs cp {POOL} out held.txt {WORK}/held.txt && [ "$(cat {WORK}/held.txt)" = first ]'
        )
        assert not err, "the file did not survive its holder being killed"
    finally:
        _kill(cijoe, f"{WORK}/hold.pid")
        _stop(cijoe)


@xnvme_parametrize(labels=["pcie"], opts=["be"])
def test_view(cijoe, device, be_opts, cli_args):
    """The read-only FUSE view lists the pool's files and reads them through the server's queues"""

    homi_id = _served(cijoe, be_opts)
    _require_fuse(cijoe)
    env = _serve(cijoe, device, homi_id, f"--view {VIEW}")

    def check(cmd, message):
        err, _ = cijoe.run(cmd)
        assert not err, message

    stopped = False
    try:
        check(
            f'[ "$(findmnt -n -o FSTYPE {VIEW})" = fuse.xnvme-fs ]',
            "the view is not mounted as fuse.xnvme-fs",
        )
        check(
            f'[ "$(findmnt -n -o OPTIONS {VIEW} | cut -d, -f1)" = ro ]',
            "the view is not read-only",
        )
        check(f'[ -z "$(ls {VIEW})" ]', "a fresh pool's view is not empty")

        # Not a multiple of the block size, and larger than an extent, so reading it whole spans extents
        check(
            f"head -c 3000017 /dev/urandom > {WORK}/v.bin && head -c 4096 /dev/urandom > {WORK}/w.bin"
            f" && {env} xnvme fs cp {POOL} in {WORK}/v.bin v.db"
            f" && {env} xnvme fs cp {POOL} in {WORK}/w.bin sub/w.db",
            "could not write the files",
        )
        check(
            f"[ \"$(ls {VIEW} | tr '\\n' ' ')\" = 'sub v.db ' ]",
            "the view does not list 'sub' and 'v.db'",
        )
        check(
            f'[ -d {VIEW}/sub ] && [ "$(ls {VIEW}/sub)" = w.db ]',
            "the nested file is not under sub/",
        )
        check(
            f'[ "$(stat -c %s {VIEW}/v.db)" = 3000017 ]', "v.db does not show its size"
        )
        check(
            f"cmp {VIEW}/v.db {WORK}/v.bin",
            "v.db reads other bytes through the view than were written",
        )
        check(
            f"cmp {VIEW}/sub/w.db {WORK}/w.bin",
            "w.db reads other bytes through the view than were written",
        )
        check(
            f"! touch {VIEW}/new 2>/dev/null", "creating a file in the view succeeded"
        )
        check(
            f"! sh -c 'printf x >> {VIEW}/v.db' 2>/dev/null",
            "appending to a file in the view succeeded",
        )
        check(f"[ ! -e {VIEW}/nope ]", "a name that does not exist was found")
        check(f"df {VIEW}", "statfs on the view failed")

        # The kernel keeps a file's attributes for a minute; the server has it drop them when the size changes, so
        # a file that grew shows its new size at once
        check(
            f"head -c 7000001 /dev/urandom > {WORK}/v.bin && {env} xnvme fs cp {POOL} in {WORK}/v.bin v.db"
            f' && [ "$(stat -c %s {VIEW}/v.db)" = 7000001 ]',
            "v.db does not show its new size after growing",
        )

        _stop(cijoe)
        stopped = True
        check(
            f"grep -q 'xnvme-fs: stopped' {WORK}/serve.log",
            "xnvme fs serve did not stop cleanly",
        )
        check(
            f"! findmnt {VIEW}", "the view stayed mounted after xnvme fs serve stopped"
        )
    finally:
        if not stopped:
            _stop(cijoe)
        cijoe.run(f"umount -l {VIEW} 2>/dev/null; true")


@xnvme_parametrize(labels=["pcie"], opts=["be"])
def test_crash(cijoe, device, be_opts, cli_args):
    """
    A writer, and in some rounds the server first, killed at random points while records are synced, checkpointed and
    renamed: the pool then holds every acknowledged record and no gap

    The processes die, the machine does not: what this covers is the order of writes, table commits and renames, not
    a lost write cache.
    """

    homi_id = _served(cijoe, be_opts)
    env = _serve(cijoe, device, homi_id, "--trace")
    access = _access([device["uri"]], homi_id)
    crash = f"{env} xnvme test fs-crash {POOL}"

    seed = random.randrange(1 << 32)
    rng = random.Random(seed)
    cijoe.run(f"echo 'test_crash: seed {seed}'")

    most = 0
    try:
        for number in range(1, 21):
            seconds = rng.randrange(300, 2800) / 1000
            both = rng.randrange(4) == 0
            what = "the server and the writer" if both else "the writer"

            # The front end execs the test, so the pid is the writer's
            kill_server = f"kill -9 $(cat {WORK}/serve.pid);" if both else ""
            cijoe.run(
                f"{crash} write 20000 > {WORK}/acks 2> {WORK}/crash.err & victim=$!; sleep {seconds};"
                f" {kill_server} kill -9 $victim; wait $victim; true"
            )
            if both:
                _kill(cijoe, f"{WORK}/serve.pid")
                _start(cijoe, POOL, access, "--trace")
            else:
                # With the server up throughout, nothing but the kill may stop the writer
                err, _ = cijoe.run(f"cat {WORK}/crash.err; [ ! -s {WORK}/crash.err ]")
                assert (
                    not err
                ), f"round {number}: the writer failed before it was killed"

            err, state = cijoe.run(
                f"echo acked $(sed -n 's/^ack //p' {WORK}/acks | tail -n 1);"
                f" holds=$({crash} check) && echo holds $holds"
            )
            if err:
                cijoe.run(f"tail -n 300 {WORK}/serve.log")
            assert (
                not err
            ), f"round {number}: the store did not reopen after killing {what}"

            acked = re.search(r"^acked ?(\d*)$", state.output(), re.M).group(1)
            holds = re.search(r"^holds (\d+)$", state.output(), re.M).group(1)
            assert int(holds) >= int(acked or 0), (
                f"round {number}, killed {what} after {seconds} s:"
                f" record {acked} was acknowledged, the store ends at {holds}"
            )
            most = max(most, int(acked or 0))
    finally:
        _stop(cijoe)

    # A writer that never acknowledges a record passes every round above
    assert most, "no round acknowledged a record, so none checked that one is kept"


@xnvme_parametrize(labels=["pcie"], opts=["be"])
def test_stats(cijoe, device, be_opts, cli_args):
    """The I/O statistics a client samples with XNVME_FS_STATS count its reads and writes per member"""

    homi_id = _served(cijoe, be_opts)
    env = _serve(cijoe, device, homi_id)

    try:
        err, state = cijoe.run(
            f"{env} XNVME_FS_STATS={WORK}/stats xnvme test fs-capi {POOL} > /dev/null && cat {WORK}/stats"
        )
        assert not err
    finally:
        _stop(cijoe)

    # time pid member samples busy read_busy depth read_depth reads writes read_bytes write_bytes, each second and at
    # the end; then per member and latency class: time pid member "latency" class bucket...
    last = {}
    timed = {}
    for fields in (line.split() for line in state.output().splitlines()):
        if len(fields) > 5 and fields[3] == "latency":
            timed[fields[2]] = timed.get(fields[2], 0) + sum(int(n) for n in fields[5:])
        elif len(fields) == 12:
            last[fields[2]] = [int(n) for n in fields[3:]]

    assert last, "the client left no statistics"
    for member, counts in last.items():
        samples, _, _, _, _, reads, writes, read_bytes, write_bytes = counts
        assert (
            samples and reads and writes
        ), f"member {member} counts no samples, reads or writes: {counts}"
        assert (
            read_bytes >= reads * 512 and write_bytes >= writes * 512
        ), f"member {member} counts too few bytes"
        assert (
            timed.get(member) == reads
        ), f"member {member}: {timed.get(member)} of {reads} reads are timed"


@xnvme_parametrize(labels=["pcie"], opts=["be"])
def test_trace(cijoe, device, be_opts, cli_args):
    """The operations clients ask of the pool, traced by the library into the file XNVME_FS_TRACE names"""

    homi_id = _served(cijoe, be_opts)
    env = _serve(cijoe, device, homi_id)
    env = f"{env} XNVME_FS_TRACE={WORK}/trace"

    # Two processes append to the one file
    try:
        err, state = cijoe.run(
            f"{env} xnvme test fs-capi {POOL} > /dev/null && {env} xnvme test fs-crash {POOL} write 60 > /dev/null"
            f" && cat {WORK}/trace"
        )
        assert not err
    finally:
        _stop(cijoe)

    # seconds pid thread operation name offset-or-size bytes
    count = {}
    nbytes = {}
    pids = set()
    for line in state.output().splitlines():
        fields = line.split()
        assert len(fields) == 7, f"not a line of the trace: {line}"
        op = fields[3]
        pids.add(fields[1])
        count[op] = count.get(op, 0) + 1
        nbytes[op] = nbytes.get(op, 0) + int(fields[6])

    assert (
        len(pids) == 2
    ), f"the trace holds the lines of {len(pids)} processes, not of two"
    for op in [
        "open",
        "close",
        "read",
        "write",
        "sync",
        "truncate",
        "stat",
        "rename",
        "lease",
        "commit",
    ]:
        assert count.get(op), f"the trace holds no '{op}'"
    assert (
        nbytes["read"] and nbytes["write"]
    ), "the trace counts no bytes read or written"


@xnvme_parametrize(labels=["pcie", "few_queues"], opts=["be"])
def test_queues(cijoe, device, be_opts, cli_args):
    """
    A controller with fewer I/O queues than threads asking for one, as consumer drives give: the error says that the
    process already holds queues there and that the threads need a cap

    The device labelled "few_queues" has two I/O queues, and is in no pool.
    """

    homi_id = _served(cijoe, be_opts)
    poke = f"{POKE} {device['uri']} {homi_id} 0 --read 4096"

    # The main thread may hold a queue from opening the member, so one reading thread is what is sure to fit in two
    err, _ = cijoe.run(f"{poke} --threads 1 > /dev/null")
    assert not err, "one thread could not read through a controller with two queues"

    err, state = cijoe.run(f"{poke} --threads 4 2>&1 > /dev/null")
    assert err, "four threads each found a queue on a controller with two"
    assert re.search(
        rf"cannot create a queue on '{device['uri']}'.*this process holds .* there already.*cap the threads",
        state.output(),
        re.S,
    ), "the error does not say the controller ran out of queues"


@xnvme_parametrize(labels=["pcie", "fs"], opts=["be"])
def test_partition(cijoe, device, be_opts, cli_args):
    """
    'mkfs.xnvme-fs' and 'mount.xnvme-fs' on a GPT partition of a controller, beside a partition with ext4 on it:
    the pool formatted, mounted, written, unmounted and mounted again with the file kept; partitions of another type
    or past the table refused; and the ext4 partition's bytes unchanged
    """

    homi_id = _served(cijoe, be_opts)
    _require(cijoe, "sfdisk", "wipefs", "mkfs.ext4")
    _require_fuse(cijoe)
    (bdf,) = _controllers(1)
    env = f"XNVME_FS_SOCKET_DIR={RUN} XNVME_FS_HOMI_ID={homi_id}"
    mkfs = f"{env} {WORK}/mkfs.xnvme-fs -L parts"
    _workdir(cijoe)

    try:
        (dev,) = _to_kernel(cijoe, [bdf])
        (offset, nbytes), _, _ = _partition(
            cijoe, dev, [(64, GPT_LINUX), (512, GPT_FS), (64, GPT_RAID)]
        )
        err, _ = cijoe.run(
            f"mkfs.ext4 -q -F {dev}p1 && mkdir {WORK}/ext4 && mount {dev}p1 {WORK}/ext4"
            f" && echo kept > {WORK}/ext4/kept && umount {WORK}/ext4"
        )
        assert not err, "could not make the ext4 partition"
        sha256 = _word(cijoe, f"sha256sum {dev}p1 | cut -d' ' -f1")
        _to_homi(cijoe, be_opts)

        _fails(cijoe, f"{mkfs} {bdf}/p1", "is not an xnvme fs partition")
        _fails(cijoe, f"{mkfs} {bdf}/p3", "is not an xnvme fs partition")
        _fails(cijoe, f"{mkfs} {bdf}/p1000", "entries")

        _mounted_pool_keeps_a_file(cijoe, env, f"{bdf}/p2", "parts")

        got = _word(
            cijoe,
            f"{POKE} {bdf} {homi_id} {offset} --read {nbytes} | sha256sum | cut -d' ' -f1",
        )
        assert got == sha256, "the ext4 partition changed"
    finally:
        cijoe.run(f"umount {VIEW} 2>/dev/null; true")
        _unpartition(cijoe, [bdf])


@xnvme_parametrize(labels=["pcie", "fs"], opts=["be"])
def test_stripe(cijoe, device, be_opts, cli_args):
    """
    A pool member that is an md RAID 0 set, made with mdadm on a Linux RAID partition of each of two controllers:
    the bytes the kernel wrote through the array read back through xnvme fs, damaged and foreign md superblocks and
    the sets xnvme fs cannot map refused, the set formatted, mounted and written, many threads' queues on it at once,
    and the bytes xnvme fs wrote read back through the kernel's array
    """

    homi_id = _served(cijoe, be_opts)
    _require(cijoe, "sfdisk", "wipefs", "mdadm")
    _require_fuse(cijoe)
    c0, c1 = _controllers(2)
    env = f"XNVME_FS_SOCKET_DIR={RUN} XNVME_FS_HOMI_ID={homi_id}"
    mkfs = f"{env} {WORK}/mkfs.xnvme-fs -L stripe -f"
    array = "/dev/md127"
    _workdir(cijoe)

    # The partitions' numbers differ between the drives, and the set is made of the last drive first, so the set's
    # order differs from the member's
    stripe = f"{c0}/p1+{c1}/p2"
    not_md = f"{c1}/p1"
    mirror = f"{c0}/p2+{c1}/p3"
    unequal = f"{c0}/p3+{c1}/p4"
    stale = f"{c0}/p4+{c1}/p5"
    other_set = f"{c0}/p1+{c1}/p5"

    def poke(member, args):
        return f"{POKE} {member} {homi_id} {args}"

    def poked(member, args):
        err, _ = cijoe.run(poke(member, args))
        assert not err

    def le32(controller, offset):
        return int(
            _word(
                cijoe,
                poke(controller, f"{offset} --read 4 | od -An -tu4 --endian=little"),
            )
        )

    def hex32(value):
        return (value & 0xFFFFFFFF).to_bytes(4, "little").hex()

    try:
        d0, d1 = _to_kernel(cijoe, [c0, c1])
        raid = [(256, GPT_RAID), (64, GPT_RAID), (64, GPT_RAID), (64, GPT_RAID)]
        ranges0 = _partition(cijoe, d0, raid)
        ranges1 = _partition(
            cijoe,
            d1,
            [(256, GPT_FS), (256, GPT_RAID), (64, GPT_RAID), (96, GPT_RAID)] + raid[3:],
        )
        _md_create(cijoe, array, [f"{d1}p2", f"{d0}p1"], opts="--chunk=64")
        size = _md_pattern(cijoe, array, "kernel.bin")
        # The sets xnvme fs refuses: RAID 1, RAID 0 of drives of unequal size, and RAID 0 that is sound until its
        # superblocks are made to disagree below; md never rewrites the superblocks of a RAID 0 set while it runs
        for number, (level, devs) in enumerate(
            [
                (1, [f"{d0}p2", f"{d1}p3"]),
                (0, [f"{d0}p3", f"{d1}p4"]),
                (0, [f"{d0}p4", f"{d1}p5"]),
            ]
        ):
            _md_create(cijoe, f"/dev/md12{number}", devs, level)
            err, _ = cijoe.run(f"mdadm --stop /dev/md12{number}")
            assert not err
        _to_homi(cijoe, be_opts)

        # An md superblock of format 1.2 lies 4 KiB into its partition
        md_super0 = ranges0[0][0] + 4096
        stale_super1 = ranges1[4][0] + 4096

        _poke_holds(cijoe, poke(stripe, ""), PATTERN_BLOCK * 4096, "kernel.bin")

        _fails(cijoe, f"{mkfs} {c0}/p1+{c0}/p1", "is named twice")
        _fails(cijoe, poke(f"{c0}/p1+{c0}/p1", "0 --read 4096"), "hold the same place")
        _fails(cijoe, f"{mkfs} {c0}/p1+{not_md}", "holds no md superblock")
        _fails(cijoe, f"{mkfs} {c0}/p1", "is not an xnvme fs partition")
        _fails(cijoe, poke(mirror, "0 --read 4096"), "of level 1; only RAID 0")
        _fails(cijoe, poke(unequal, "0 --read 4096"), "differ in size")

        # One more event on the last drive's superblock, its checksum, a sum of little-endian words, moved on with it
        events = stale_super1 + 200
        csum = stale_super1 + 216
        events_was = le32(c1, events)
        csum_was = le32(c1, csum)
        poked(c1, f"{events} {hex32(events_was + 1)}")
        poked(c1, f"{csum} {hex32(csum_was + 1)}")
        _fails(cijoe, poke(stale, "0 --read 4096"), "disagree: events")
        poked(c1, f"{events} {hex32(events_was)}")
        poked(c1, f"{csum} {hex32(csum_was)}")
        poked(stale, "0 --read 4096 > /dev/null")
        _fails(cijoe, poke(other_set, "0 --read 4096"), "belong to different md sets")

        # A byte of the set's name in the superblock of one drive, flipped and then put back
        name = md_super0 + 32
        was = int(_word(cijoe, poke(c0, f"{name} --read 1 | od -An -tu1")))
        poked(c0, f"{name} {was ^ 0xFF:02x}")
        _fails(cijoe, f"{mkfs} {stripe}", "checksum mismatch")
        poked(c0, f"{name} {was:02x}")

        _mounted_pool_keeps_a_file(cijoe, env, stripe, "stripe")

        # The homi server's one heap serves every thread's queue on every drive, and a queue takes its slots as its
        # commands need them: 24 threads on both drives of the set, then 48 threads each with all of a queue's
        # commands in flight
        for member, nbytes, threads in [(stripe, 131072, 24), (not_md, 4 * MIB, 48)]:
            read = poke(member, f"0 --read {nbytes}")
            err, _ = cijoe.run(
                f'[ "$({read} | sha256sum)" = "$({read} --threads {threads} | sha256sum)" ]'
                f' && [ "$({read} | wc -c)" = {nbytes} ]'
            )
            assert not err, f"{threads} threads on {member} read other bytes than one"

        offset = size - PATTERN_NBYTES - 12288
        _poke_write(cijoe, poke(stripe, ""), offset, "fs.bin")

        devs = [f"{d}{p}" for d, p in zip(_to_kernel(cijoe, [c0, c1]), ["p1", "p2"])]
        _md_holds(cijoe, array, devs, offset, "fs.bin")
    finally:
        cijoe.run(f"umount {VIEW} 2>/dev/null; true")
        _unpartition(cijoe, [c0, c1])


@xnvme_parametrize(labels=["pcie", "fs"], opts=["be"])
def test_stripe_wide(cijoe, device, be_opts, cli_args):
    """
    An md RAID 0 set of a partition of each of four controllers, named in another order than mdadm made the set
    from: what the kernel wrote reads back through xnvme fs, a pool on the set keeps a file, and what xnvme fs wrote
    reads back through the kernel's array
    """

    homi_id = _served(cijoe, be_opts)
    _require(cijoe, "sfdisk", "wipefs", "mdadm")
    _require_fuse(cijoe)
    bdfs = _controllers(4)
    env = f"XNVME_FS_SOCKET_DIR={RUN} XNVME_FS_HOMI_ID={homi_id}"
    array = "/dev/md125"
    poke = f"{POKE} {'+'.join(f'{bdf}/p1' for bdf in bdfs)} {homi_id}"
    _workdir(cijoe)

    try:
        parts = [f"{dev}p1" for dev in _to_kernel(cijoe, bdfs)]
        for part in parts:
            _partition(cijoe, part[: -len("p1")], [(128, GPT_RAID)])
        _md_create(
            cijoe, array, [parts[3], parts[0], parts[2], parts[1]], opts="--chunk=64"
        )
        size = _md_pattern(cijoe, array, "kernel.bin")
        _to_homi(cijoe, be_opts)

        _poke_holds(cijoe, poke, PATTERN_BLOCK * 4096, "kernel.bin")
        _mounted_pool_keeps_a_file(
            cijoe, env, "+".join(f"{bdf}/p1" for bdf in bdfs), "wide"
        )
        offset = size - PATTERN_NBYTES - 12288
        _poke_write(cijoe, poke, offset, "fs.bin")

        parts = [f"{dev}p1" for dev in _to_kernel(cijoe, bdfs)]
        _md_holds(cijoe, array, parts, offset, "fs.bin")
    finally:
        cijoe.run(f"umount {VIEW} 2>/dev/null; true")
        _unpartition(cijoe, bdfs)


@xnvme_parametrize(labels=["pcie", "fs"], opts=["be"])
def test_stripe_whole(cijoe, device, be_opts, cli_args):
    """
    An md RAID 0 set of two whole namespaces, with mdadm's default chunk: what the kernel wrote reads back through
    xnvme fs, and what xnvme fs wrote reads back through the kernel's array
    """

    homi_id = _served(cijoe, be_opts)
    _require(cijoe, "wipefs", "mdadm")
    c0, c1 = _controllers(2)
    array = "/dev/md124"
    poke = f"{POKE} {c0}+{c1} {homi_id}"
    _workdir(cijoe)

    try:
        d0, d1 = _to_kernel(cijoe, [c0, c1])
        _wipe(cijoe, [d0, d1])
        _md_create(cijoe, array, [d1, d0])
        size = _md_pattern(cijoe, array, "kernel.bin")
        _to_homi(cijoe, be_opts)

        _poke_holds(cijoe, poke, PATTERN_BLOCK * 4096, "kernel.bin")
        offset = size - PATTERN_NBYTES - 12288
        _poke_write(cijoe, poke, offset, "fs.bin")

        _md_holds(cijoe, array, _to_kernel(cijoe, [c0, c1]), offset, "fs.bin")
    finally:
        _unpartition(cijoe, [c0, c1])


@xnvme_parametrize(labels=["bdev", "fs"], opts=["be"])
def test_stripe_kernel(cijoe, device, be_opts, cli_args):
    """
    An md RAID 0 set of two whole namespaces reached through the kernel's nvme driver, for each backend that does:
    what the kernel wrote through the array reads back through xnvme fs, what xnvme fs wrote reads back through the
    array, and a pool on the set keeps a file
    """

    _built(cijoe)
    if be_opts["be"] != "io_uring":
        pytest.skip(
            reason="Runs once, for the block devices; it names the backends itself"
        )
    _require(cijoe, "wipefs", "mdadm")
    blocks = [d["uri"] for d in cijoe_config_get_all_devices(["bdev", "fs"])][:2]
    if len(blocks) < 2:
        pytest.skip(reason="Requires 2 devices labelled: ['bdev', 'fs']")
    array = "/dev/md126"

    try:
        for backend in ["io_uring", "io_uring_direct", "io_uring_cmd"]:
            # io_uring_cmd sends NVMe commands to the namespace's character device
            devs = (
                [b.replace("/dev/nvme", "/dev/ng") for b in blocks]
                if backend == "io_uring_cmd"
                else blocks
            )
            member = "+".join(devs)
            env = f"XNVME_FS_BACKEND={backend} XNVME_FS_SOCKET_DIR={WORK}"
            poke = f"XNVME_FS_BACKEND={backend} {POKE} {member} 0"
            access = f"--member {member} --backend {backend}"
            _workdir(cijoe)

            cijoe.run("udevadm settle; mdadm --stop --scan; true")
            _wipe(cijoe, blocks)
            # The set is made of the last drive first, so the set's order differs from the member's
            _md_create(cijoe, array, blocks[::-1], opts="--chunk=64")
            size = _md_pattern(cijoe, array, "kernel.bin")

            _poke_holds(cijoe, poke, PATTERN_BLOCK * 4096, "kernel.bin")
            offset = size - PATTERN_NBYTES - 12288
            _poke_write(cijoe, poke, offset, "fs.bin")
            _md_holds(cijoe, array, blocks, offset, "fs.bin")

            err, _ = cijoe.run(
                f"xnvme fs mkfs --pool {POOL} {access} --extent-size 1M --overwrite"
            )
            assert not err, f"xnvme fs mkfs failed through {backend}"
            _start(cijoe, POOL, access)
            try:
                err, _ = cijoe.run(
                    f"head -c 5242883 /dev/urandom > {WORK}/in.bin"
                    f" && {env} xnvme fs cp {POOL} in {WORK}/in.bin t.db"
                    f" && {env} xnvme fs cp {POOL} out t.db {WORK}/out.bin"
                    f" && cmp {WORK}/in.bin {WORK}/out.bin"
                )
                assert not err, f"a pool on the set lost a file through {backend}"
            finally:
                _stop(cijoe)
    finally:
        cijoe.run("mdadm --stop --scan; true")
        _wipe(cijoe, blocks)
