:orphan:

.. _sec-fs:

xnvme fs
########

**xnvme fs** keeps files on NVMe controllers. A *pool* is a set of members,
striped over in extents, and named by its files and directories. ``xnvme fs
serve`` serves the metadata of one pool on a Unix socket: the names, the sizes
and which extents each file holds. The data does not pass through the server.
Each client process reads and writes the extents of its files itself, with
queues of its own on the members, through the **uPCIe** backend and a HOMI
server, ``xnvme homi serve``, or through the kernel's ``io_uring``.

Clients take **xnvme fs** through the C interface in ``libxnvme_fs.h``, which
``libxnvme-fs`` implements. ``libxnvme-fs`` is written in C++17 and is a
shared library of its own, so that ``libxnvme`` does not link the C++ runtime.

``xnvme fs --help`` prints the usage.

Requirements
============

**xnvme fs** is Linux-only and needs a C++17 compiler to build. At runtime it
requires:

* for members reached through **uPCIe**, a HOMI server holding the
  controllers, and the HOMI id it serves them under

* for members reached through the kernel, the namespaces' block devices, or
  their character devices for ``io_uring_cmd``

* ``/dev/fuse``, only for the view that ``--view`` and ``mount.xnvme-fs`` mount

Members and access
==================

A member is named by ``--member``, once for each member, in the order the pool
was made with. With the ``upcie`` backend, the default, a member is one of:

* a controller held by the HOMI server, by PCI address, such as
  ``0000:03:00.0``

* partition ``K`` of such a controller's GPT, as ``0000:03:00.0/pK``, of the
  partition type ``a04faa88-135a-4457-b8f6-db8bbaf344b8``

* the drives of an md RAID 0 set made with ``mdadm``, joined by ``+``, as
  ``0000:03:00.0+0000:04:00.0``

``--homi-id N`` names the HOMI server, and defaults to ``XNVME_FS_HOMI_ID``.

With ``--backend io_uring`` or ``--backend io_uring_direct``, a member is a
namespace's block device, such as ``/dev/nvme0n1``, and with ``--backend
io_uring_cmd`` its character device, such as ``/dev/ng0n1``. These reach the
members through the kernel's nvme driver, and take no HOMI id. The backend
defaults to ``XNVME_FS_BACKEND``, else ``upcie``. The server and every client
of a pool use the same backend.

``mkfs``: Make a pool
=====================

Formats the members as a pool::

   xnvme fs mkfs --pool scan --member 0000:03:00.0 --member 0000:04:00.0 --homi-id 1

``--extent-size`` sets the size of an extent, with a ``K``, ``M``, ``G`` or
``T`` suffix. ``mkfs`` fails on a member that already holds a pool, unless
``--overwrite`` is given.

``serve``: Serve a pool
=======================

Opens the pool and serves it until ``SIGINT`` or ``SIGTERM``, upon which it
commits the pool's metadata and closes its members::

   xnvme fs serve --pool scan --member 0000:03:00.0 --member 0000:04:00.0 --homi-id 1

The socket is ``/run/xnvme-fs/POOL.sock`` unless ``--socket`` names another.
One server serves a pool, and one server holds a member: a second ``serve`` or
``mkfs`` on the same member fails. A file is open in one client process at a
time, and a second process that opens it gets ``EBUSY``.

``--view DIR`` mounts a read-only FUSE view of the pool at ``DIR``, so that
tools such as ``ls`` and ``cp`` read its files. ``--trace`` logs each request
the server handles.

``info``: Show a pool
=====================

Reads the superblock and the extent map table from the members, without a
server, and prints the pool, its members and its files::

   xnvme fs info --pool scan --member 0000:03:00.0 --member 0000:04:00.0 --homi-id 1

``cp``: Copy a file in or out
=============================

Copies a local file into a pool that is being served, or a file of the pool
out to a local one. A file copied in is synced before ``cp`` returns::

   xnvme fs cp scan in ./lineitem.parquet tpch/lineitem.parquet
   xnvme fs cp scan out tpch/lineitem.parquet ./lineitem.parquet

``cp`` is a client of the pool: it finds the server's socket in
``XNVME_FS_SOCKET_DIR``, else in ``/run/xnvme-fs``, and takes the HOMI id from
``XNVME_FS_HOMI_ID`` and the backend from ``XNVME_FS_BACKEND``.

``mkfs.xnvme-fs`` and ``mount.xnvme-fs``
========================================

Under these names, ``xnvme-fs`` takes the command lines that ``mkfs(8)`` and
``mount(8)`` pass their helpers. ``mount.xnvme-fs MEMBER DIR`` reads the pool
and its other members from the superblock of ``MEMBER``, and leaves a server
running that serves the pool and its view at ``DIR`` until the view is
unmounted.

Running as a service
====================

A unit template, ``xnvme-fs@.service``, is installed where systemd is
present, one instance per pool, along with an example configuration::

   cp /etc/xnvme/fs.conf.example /etc/xnvme/fs-scan.conf
   systemctl enable --now xnvme-fs@scan

The instance serves the pool ``scan`` at ``/run/xnvme-fs/scan.sock``, and
``systemctl start`` returns once the socket is open. The unit does not make
the pool; run ``xnvme fs mkfs`` first. It reads these from
``/etc/xnvme/fs-POOL.conf``:

``XNVME_FS_MEMBER``
   The pool's members, space separated.

``XNVME_FS_HOMI_ID``
   The HOMI server holding the members. Leave it out for a kernel backend.

``XNVME_FS_BACKEND``
   The backend, as for ``--backend``. Leave it out for ``upcie``.

``XNVME_FS_OPTS``
   Anything else for ``xnvme fs serve``, such as ``--trace`` or ``--view DIR``.

The unit is not ordered after the HOMI server holding the members, since a
unit cannot name another unit from its configuration; add the ordering with a
drop-in.

Environment of a client
=======================

``libxnvme-fs`` reads these in the process that uses it, which is a client or
the server:

``XNVME_FS_BACKEND``
   The backend the members are reached with where the caller names none:
   ``upcie``, ``io_uring``, ``io_uring_direct`` or ``io_uring_cmd``. The
   default is ``upcie``.

``XNVME_FS_APPEND_BUFFER``
   Set to anything but ``0``, a file holds appends smaller than one command in
   memory until it is synced, closed, or the buffer fills. A process that dies
   loses the appends it held.

``XNVME_FS_POLL_MODE``
   How a thread waits for its commands: ``paced``, the default, sleeps for
   half of what its earlier waits took before it polls; ``spin`` polls without
   sleeping. Any other value fails the call that waits.

``XNVME_FS_STATS``
   A file to which the client appends, each second and at the end, a line per
   member with its commands in flight and its reads and writes completed.

``XNVME_FS_TRACE``
   A file to which the client appends a line for each operation: the time,
   the process, the thread, the operation, the name, and an offset and a
   length.

``XNVME_FS_HOMI_ID`` and ``XNVME_FS_SOCKET_DIR`` are read by ``xnvme fs``
itself, as described above, not by the library: a program that uses
``libxnvme_fs.h`` passes both to ``xnvme_fs_connect()``.
