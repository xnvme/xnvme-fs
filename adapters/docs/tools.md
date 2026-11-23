<!--
SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>

SPDX-License-Identifier: CC-BY-4.0
-->

# Tools

All three are one program, `xnvme-fs`, which `xnvme fs` runs. Called as `mkfs.xnvme-fs` or
`mount.xnvme-fs`, it takes the command lines `mkfs(8)` and `mount(8)` pass the
helpers they find by name, so `mkfs -t xnvme-fs` and `mount -t xnvme-fs` work
once the two names are on the path.

A member is a controller held by HOMI, `0000:84:00.0`, or partition N of its
GPT, `0000:84:00.0/pN`, which must have xnvme fs's type,
`a04faa88-135a-4457-b8f6-db8bbaf344b8`, or an md RAID 0 set made with mdadm,
named by its drives joined by `+`, `0000:82:00.0+0000:83:00.0/p1`, each a
controller or a partition of the type Linux RAID. Sizes take a K, M, G or T
suffix. The
HOMI id comes from the command line or from `XNVME_FS_HOMI_ID`.

## mkfs.xnvme-fs

    mkfs.xnvme-fs [-L NAME] [-E EXTENT_SIZE] [-f] [-H HOMI_ID] MEMBER...

Writes a superblock to every member, naming the pool and all its members, and
an empty table. The pool is named `xnvme-fs` unless `-L` names it. Extents are
64 MiB unless `-E` says otherwise; they are the unit of allocation and of
striping across members. A member that already holds a pool is refused unless
`-f` is given. It prints each member with its size and number of extents.

## mount.xnvme-fs

    mount.xnvme-fs MEMBER DIR [-o homi=N,socket=PATH,trace]

Reads the pool's name and its other members from MEMBER's superblock, starts
`xnvme fs serve` for the pool in the background with the read-only view at DIR, and
returns once the view is mounted, or fails with what stopped the daemon. The
daemon serves the metadata socket, by default `/run/xnvme-fs/NAME.sock`, logs to
`/run/xnvme-fs/NAME.log`, and stops when DIR is unmounted. Options that
`mount(8)` passes and that mean nothing to a read-only view, such as `ro` and
`defaults`, are ignored.

## xnvme fs

    xnvme fs mkfs  --pool NAME --member MEMBER... --homi-id N [--extent-size BYTES] [--overwrite]
    xnvme fs serve --pool NAME --member MEMBER... --homi-id N [--socket PATH] [--view DIR] [--trace]
    xnvme fs info  --pool NAME --member MEMBER... --homi-id N

`mkfs` is what `mkfs.xnvme-fs` does. `serve` runs in the foreground until
interrupted, serving the socket and, with `--view`, the view; `--trace` logs
every request. `info` prints the superblock's fields and every file with its
size and extents. One daemon serves a pool, and a lock under `/run/lock` keeps
a second from formatting or serving the same member.
