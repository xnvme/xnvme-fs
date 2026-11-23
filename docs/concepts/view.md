<!--
SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>

SPDX-License-Identifier: CC-BY-4.0
-->

# The view

`xnvme fs serve --view DIR` mounts a read-only view of the pool at `DIR`, and
`mount.xnvme-fs` mounts a view at the directory given to it. Programs such as
`ls` and `cp` then access the pool's directories and files as ordinary
directories and files, while the database engines that use the pool change
them. The server reads file data for the view on its own queues on the
members. The clients' I/O uses the clients' queues on the same controllers.

## How the server serves it

The server implements the FUSE protocol on `/dev/fuse` with the header-only
library ufuse. meson fetches ufuse v0.4.0 through the wrap file
`subprojects/ufuse.wrap`. The server's poll loop includes the descriptor of
`/dev/fuse`, so the server handles a request from the view between the
requests of clients, and the clients' requests wait while the server performs
a read for the view. ufuse limits one read from the kernel to 128 KiB.

The server mounts the view with `mount(2)`, which requires `CAP_SYS_ADMIN`.
The file system type is `fuse.xnvme-fs`, the source is the pool's name, the
mount is read-only, and the mount options are `allow_other` and
`default_permissions`. Before the server mounts the view, it unmounts any file
system mounted at the directory, which removes a view that remains from a
server that exited without unmounting it. When the kernel's FUSE protocol is
older than 7.39, the server closes the view.

## What the view shows

The view shows the namespace that the server has in memory: the files in the
table, the files never committed, the directories made with `MKDIR`, and the
directories that exist because the pool has names below them. A file has the
mode `0444` and a directory has the mode `0555`. The size of a file is the
size that the server last received from the file's client, at the file's
creation or at its last commit, truncate or close. The view does not show the
size in the process that writes the file.

The server assigns an inode number to each path when the kernel first looks
the path up, and the number does not change; a name that is removed and
created again has the same inode number. The kernel may cache a file's
attributes for 60 seconds, and the server notifies the kernel to drop the
cached attributes after every commit, truncate or close of the file. The
kernel may cache a directory's attributes, and the lookup of any name, for 1
second, so the view shows the addition or removal of a name within 1 second.

A read in the view reads the file's extents from the members, up to the size
that the server last received, and returns zeros for a hole. The server opens
the files of the view for direct I/O, so the kernel caches none of their pages
and every read results in commands to the drives. Appends that a client
buffers in memory are not on the drives, and a read in the view does not
return them.

`statfs()` on the view reports blocks of 4 KiB: the pool's extents as the
total, its free extents as free and available, the number of files the server
has, and 255 as the longest name.

## Requests that change the pool

The view is read-only. ufuse answers with `EROFS` to every request that would
change the pool and to every open for writing, and with `ENOSYS` to every
request that the view does not implement.

## Unmounting

`umount DIR` unmounts the view, and the server then closes the view. A server
that `mount.xnvme-fs` started exits when its view is closed. A server started
as `xnvme fs serve --view` continues to serve the socket. When the server
stops, it unmounts the view.
