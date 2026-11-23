<!--
SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>

SPDX-License-Identifier: CC-BY-4.0
-->

# Tools

The program `xnvme-fs` implements every command of xnvme fs. The xNVMe
front-end `xnvme` runs the program as `xnvme fs`, with the commands `mkfs`,
`serve`, `info` and `cp`. `mkfs(8)` and `mount(8)` run helpers named
`mkfs.xnvme-fs` and `mount.xnvme-fs` for the type `xnvme-fs`. When
`xnvme-fs` is run under one of these two names, it parses the command line
that `mkfs(8)` or `mount(8)` passes to its helper. `xnvme fs --help` prints
the usage.

## Requirements

xnvme fs runs on Linux. At run time it needs:

* for members accessed with the backend `upcie`, a HOMI server
  (`xnvme homi serve`) that serves the controllers, and the HOMI id of that
  server
* for members accessed through the kernel, the block devices of the
  namespaces, or their character devices for the backend `io_uring_cmd`
* `/dev/fuse` and `CAP_SYS_ADMIN`, only for the view that `--view` and
  `mount.xnvme-fs` mount

## What is installed

The meson project installs these files. It does not install xNVMe, which it
builds against.

| File | Where |
|--|--|
| `xnvme-fs` | xNVMe's `libexecdir`, where `xnvme` finds the programs it runs |
| `fs.extension` | xNVMe's `extensiondir`; with this file, the overview that `xnvme` prints lists `xnvme fs` in the section `serve` |
| `mkfs.xnvme-fs`, `mount.xnvme-fs` | symbolic links to `xnvme-fs` in `sbindir` |
| `libxnvme-fs.so`, `libxnvme_fs.h`, `xnvme-fs.pc` | the library, its header and its pkg-config file, in `libdir`, `includedir` and `libdir/pkgconfig` |
| `xnvme-fs@.service` | the systemd unit directory, unless the meson option `systemd` is disabled or systemd is not found |
| `fs.conf.example` | `sysconfdir/xnvme`, installed together with the unit |

## Members and access

`xnvme fs mkfs`, `serve` and `info` take the name of the pool with `--pool`
and one `--member` option for each member, in the order in which the pool
was formatted. `ACCESS` in the usage lines below stands for the options that
select how the commands access the members:

* `--homi-id N` selects the HOMI server with the id N, for the backend
  `upcie`, which is the default backend. A member is then a controller that
  the HOMI server serves, named by its PCI address, such as `0000:03:00.0`.
  A member can also be partition K of the controller's GPT, written
  `0000:03:00.0/pK`, or the drives of an md RAID 0 set joined by `+`, written
  `0000:03:00.0+0000:04:00.0`. The HOMI id defaults to the value of
  `XNVME_FS_HOMI_ID`.
* `--backend io_uring` or `--backend io_uring_direct`: a member is the block
  device of a namespace, such as `/dev/nvme0n1`, or a partition or an md set
  written in the same way.
* `--backend io_uring_cmd`: a member is the character device of a namespace,
  such as `/dev/ng0n1`, or a partition or an md set written in the same way.

The backend defaults to the value of `XNVME_FS_BACKEND`, and to `upcie` when
that variable is unset or empty. The server and every client of a pool must
use the same backend. [Pools and members](concepts/pools.md) describes the
partitions, [md RAID 0 members](concepts/md-raid0.md) describes the md sets,
and [Access](concepts/access.md) describes the backends.

A size, such as the argument of `--extent-size`, is a number of bytes. A
suffix `K`, `M`, `G` or `T` multiplies the number by 1024, 1024^2, 1024^3 or
1024^4.

## xnvme fs mkfs

```
xnvme fs mkfs --pool NAME --member MEMBER... ACCESS [--extent-size BYTES] [--overwrite]
```

Formats the members as a pool. `mkfs` writes a superblock to every member,
which names the pool and all its members, and an empty table to member 0.
The extent size is 64 MiB unless `--extent-size` gives another size, which
must be a power of two of at least 64 KiB. `mkfs` fails with `EEXIST` when a
member already contains a superblock of xnvme fs, unless `--overwrite` is
given. `mkfs` fails with `EBUSY` when a running `xnvme fs serve` or another
`xnvme fs mkfs` has locked one of the drives. `mkfs` prints one line for each
member, with the name of the member, its size in bytes and its number of
extents, separated by tabs.

```
xnvme fs mkfs --pool scan --member 0000:03:00.0 --member 0000:04:00.0 --homi-id 1
```

## xnvme fs serve

```
xnvme fs serve --pool NAME --member MEMBER... ACCESS [--socket PATH] [--view DIR] [--trace]
```

Opens the pool and serves it in the foreground until the process receives
`SIGINT` or `SIGTERM`. The server listens on the socket
`/run/xnvme-fs/POOL.sock` unless `--socket` gives another path. Only one
server can serve a pool. `serve` and `mkfs` take an exclusive lock on a file
under `/run/lock` for each drive, so a second `serve` or a `mkfs` on the same
drive fails with `EBUSY`. Only one connection to the server can have a file
open at a time. A request on another connection to open, remove or rename that
file fails with `EBUSY`.

`--view DIR` mounts a read-only view of the pool at `DIR`, so that programs
such as `ls` and `cp` can read the files of the pool.
[The view](concepts/view.md) describes the view. With `--trace`, the server
writes a line to standard error for each request `OPEN`, `CLOSE`, `LEASE`,
`COMMIT`, `TRUNCATE`, `REMOVE`, `RENAME` and `INDEX` that it handles, for
each request that fails, and for each client that disconnects.

The server writes the pool's table when a request changes the table, for
example when a client commits a file. The server does not write the table
when it stops. On `SIGINT` or `SIGTERM` the server closes the files that its
clients have open, unmounts the view, removes the socket and exits.
[The server and its protocol](concepts/server.md) describes the server
process.

```
xnvme fs serve --pool scan --member 0000:03:00.0 --member 0000:04:00.0 --homi-id 1
```

## xnvme fs info

```
xnvme fs info --pool NAME --member MEMBER... ACCESS
```

Reads the superblocks and the table from the members and prints the table
that is stored on the drives. `info` does not connect to a server and does
not take the drive locks. The fields of each output line are separated by
tabs. The first five lines give `pool`, `layout_version`, `extent_size`,
`slot_size` and `generation`, each followed by its value. Then follows one
line for each directory, with `dir` and the name of the directory, and one
line for each committed file, with `file`, the name of the file, its
committed size and its number of extents. A file that no client has committed
is not in the table, so `info` does not print it. `info` does not print the
members.

```
xnvme fs info --pool scan --member 0000:03:00.0 --member 0000:04:00.0 --homi-id 1
```

While a server serves the pool, `xnvme_fs_info()` and `xnvme_fs_members()`
of the C API, or the DuckDB function `xnvme_fs_pool_info()`, return the state
of the pool in the server.

## xnvme fs cp

```
xnvme fs cp POOL in LOCAL NAME
xnvme fs cp POOL out NAME LOCAL
```

Copies a local file into a pool that a server serves, or a file of the pool
out to a local file. `cp` copies 8 MiB at a time. `in` creates `NAME` or
truncates it when it exists, and syncs `NAME` before `cp` returns:

```
xnvme fs cp scan in ./lineitem.parquet tpch/lineitem.parquet
xnvme fs cp scan out tpch/lineitem.parquet ./lineitem.parquet
```

`cp` is a client of the pool, written in C against `libxnvme_fs.h`. `cp`
looks for the server's socket in the directory that `XNVME_FS_SOCKET_DIR`
names, or in `/run/xnvme-fs` when that variable is unset or empty. `cp` takes
the HOMI id from `XNVME_FS_HOMI_ID` and the backend from `XNVME_FS_BACKEND`.
[C API](api.md) shows the source of the copy into the pool.

## mkfs.xnvme-fs

```
mkfs.xnvme-fs [-L NAME] [-E EXTENT_SIZE] [-f] [-H N] MEMBER...
```

Formats a pool as `xnvme fs mkfs` does, and takes the options of a `mkfs`
helper. The name of the pool is `xnvme-fs` unless `-L` gives another name.
`-E` sets the extent size, and `-f` has the effect of `--overwrite`. `-H`
gives the HOMI id, which defaults to the value of `XNVME_FS_HOMI_ID`. The
backend is the value of `XNVME_FS_BACKEND`, or `upcie` when that variable is
unset or empty.

## mount.xnvme-fs

```
mount.xnvme-fs MEMBER DIR [-o homi=N,socket=PATH,trace]
```

Reads the name of the pool and its members from the superblock of `MEMBER`,
and starts a server for the pool as a background process in a new session
(`setsid(2)`), with the read-only view mounted at `DIR`. `mount.xnvme-fs`
returns when the view is mounted. When the server exits before the view is
mounted, `mount.xnvme-fs` prints the output of the server and fails. The
server listens on the socket `/run/xnvme-fs/POOL.sock` unless `socket=` gives
another path, writes its log to `/run/xnvme-fs/POOL.log`, and exits when
`DIR` is unmounted.

`homi=` gives the HOMI id, which defaults to the value of
`XNVME_FS_HOMI_ID`, and `trace` has the effect of `--trace`. The backend is
the value of `XNVME_FS_BACKEND`, or `upcie` when that variable is unset or
empty. `mount.xnvme-fs` ignores the options `-s`, `-f`, `-n`, `-v` and
`-t TYPE`, which `mount(8)` may pass, and every other `-o` option, such as
`ro` and `defaults`. `mount.xnvme-fs` fails with `EBUSY` when a view of
xnvme fs is already mounted at `DIR`.

```
mount.xnvme-fs 0000:03:00.0 /mnt/scan -o homi=1
umount /mnt/scan
```

## Running as a service

Each instance of the unit template `xnvme-fs@.service` serves one pool. The
configuration of an instance starts as a copy of the installed example:

```
cp /etc/xnvme/fs.conf.example /etc/xnvme/fs-scan.conf
systemctl enable --now xnvme-fs@scan
```

The instance `xnvme-fs@scan` serves the pool `scan` on the socket
`/run/xnvme-fs/scan.sock`. `systemctl start` returns when the socket exists,
and fails when the socket does not exist after 10 seconds. The unit does not
format the pool, so the pool must be formatted with `xnvme fs mkfs` before
the unit is started. The unit reads these variables from
`/etc/xnvme/fs-POOL.conf`:

`XNVME_FS_MEMBER`
: The pool's members, separated by spaces.

`XNVME_FS_HOMI_ID`
: The id of the HOMI server that serves the members. When the variable is not
  set, as for a kernel backend, the unit passes no `--homi-id`.

`XNVME_FS_BACKEND`
: The backend, with the values of `--backend`. When the variable is not set,
  the backend is `upcie`.

`XNVME_FS_OPTS`
: Additional options for `xnvme fs serve`, such as `--trace` or `--view DIR`.

systemd restarts a server that fails after 5 seconds, and starts the unit at
most 3 times in 180 seconds. systemd waits up to 30 seconds for a server to
stop. The unit has no ordering after the HOMI server that serves the members,
because a unit cannot name another unit from its configuration file. A
drop-in adds the ordering, for example `After=xnvme-homi@1.service`, where
`xnvme-homi@.service` is the unit that xNVMe installs for a HOMI server.

## Environment

`libxnvme-fs` reads these variables in every process that uses the library,
including `xnvme fs serve`:

`XNVME_FS_BACKEND`
: The backend that accesses the members when the caller gives none: `upcie`,
  `io_uring`, `io_uring_direct` or `io_uring_cmd`. The default is `upcie`.

`XNVME_FS_POLL_MODE`
: How a thread waits for its commands to complete. With `paced`, the default,
  the thread first sleeps for half of the mean duration of its earlier waits
  for requests of a similar size, and then polls. With `spin`, the thread
  polls for the whole wait. With any other value, the call that waits fails
  with `EINVAL`.

`libxnvme-fs` reads these variables only in a client, which is a process that
creates a session with `xnvme_fs_connect()`:

`XNVME_FS_APPEND_BUFFER`
: When the variable is set to a value other than the empty string or `0`, a
  file keeps appends that are smaller than one command in memory. The library
  writes them at a sync, at a truncate, at the close of the file, or when the
  buffer is full. A process that terminates before that loses the appends
  held in memory.

`XNVME_FS_STATS`
: The path of a file to which each session appends statistics, every second
  and when the session ends. The session samples the commands in flight on
  each member every millisecond. Each line describes one member and has
  these fields: the time, the process id, the index of the member, the number
  of samples, the number of samples with a command in flight, the number of
  samples with a read in flight, the sum over the samples of the commands in
  flight, the sum over the samples of the reads in flight, and the totals of
  reads, writes, bytes read and bytes written. When the session ends, the
  session also appends one line for each member and read latency class, with
  the time, the process id, the index of the member, the word `latency`, the
  class, and 16 counts.

`XNVME_FS_TRACE`
: The path of a file to which the library appends one line for each
  operation. The line has the seconds since the first traced operation of the
  process, the process id, the thread id, the operation, the name of the file
  (`/` when the operation has no file), and two numbers that depend on the
  operation, such as an offset and a length.

The library does not read `XNVME_FS_HOMI_ID` and `XNVME_FS_SOCKET_DIR`. A
program written against `libxnvme_fs.h` passes both values to
`xnvme_fs_connect()`. `xnvme fs`, `xnvme fs cp` and the adapters read
`XNVME_FS_HOMI_ID`. `xnvme fs cp` and the adapters read
`XNVME_FS_SOCKET_DIR`, which defaults to `/run/xnvme-fs`.

## Exit status

A command exits with 0 on success. When a command fails, it prints the
failure to standard error and exits with 1. For a command line that it cannot
parse, a command prints the usage to standard error and exits with 64. With
`--help`, a command prints the usage to standard output and exits with 0.
`xnvme fs serve` exits with 2 when writing the pool's table fails with an
error other than `ENOSPC`, which is the error for a table that does not fit
its slot.
