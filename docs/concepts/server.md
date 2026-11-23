<!--
SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>

SPDX-License-Identifier: CC-BY-4.0
-->

# The server and its protocol

`xnvme fs serve` serves one pool. It opens the members, reads the table into
memory, and answers requests from clients on a Unix socket. The server changes
the pool's metadata and writes the table only while it handles a request. The
server does not read or write file data for a client; each client submits its
own I/O to the members.

## The process

`xnvme fs serve` runs one thread. A loop polls the listening socket, the
descriptor of the [view](view.md) when a view is mounted, and the connection
of every client. The loop handles each request to completion before it handles
the next, including the table writes and the reads for the view that a request
causes. A request therefore waits for the request before it.

Before the server opens the pool, it takes an exclusive `flock()` lock for
every drive of every member. The lock file is `/run/lock/xnvme-fs-NAME.lock`,
where `NAME` is the drive's name with each `/` replaced by `-`. The start
fails with `EBUSY` when another `xnvme fs serve` or `xnvme fs mkfs` has the
lock of a drive, and with `EINVAL` when a drive is named twice.
`xnvme fs mkfs` takes the same locks; `xnvme fs info` takes none.

The socket is `/run/xnvme-fs/POOL.sock` unless `--socket` names another path.
The server creates the socket's directory when the directory does not exist,
but not the parents of the directory. When a server accepts a connection on
the path, the start fails with `EADDRINUSE`. A socket file on which no server
accepts a connection remains from a server that exited without removing it,
and the new server removes the file.

On `SIGINT` or `SIGTERM`, the server closes the handles of every connection as
it does when a connection ends, unmounts the view, removes the socket and
releases its locks. The server does not write the table when it stops, because
it wrote every committed change at the time of the commit.

With `--trace`, the server writes a line to its standard error for every
`OPEN`, `CLOSE`, `LEASE`, `COMMIT`, `TRUNCATE`, `REMOVE`, `RENAME` and `INDEX`
it handles, for every request that fails, and for every connection that ends.

## Connections

`xnvme_fs_connect()` creates a session of `libxnvme-fs`, and a session has one
connection. The library sends one request at a time on the connection, from
whichever thread makes the call, and waits for the answer. A connection fails
on a short read or write, on an incomplete frame, or when the server closes
it. The library does not reconnect: every later call on the session fails with
`ENOTCONN`, and the program must create a new session.

## Frames and encodings

Every request and every answer is a frame: a 32-bit length, then a body of
that many bytes, at most 64 MiB (`MAX_FRAME`). All integers are little-endian.
A string is a 32-bit length and that many bytes, with no NUL byte. A list of
extents is a 32-bit count and one 64-bit address per extent.

The body of a request is an 8-bit operation code and the arguments of the
operation. The body of an answer starts with a 32-bit status. A status of 0 is
followed by the results of the operation. Any other status is an errno value
and is followed by a string with the server's message. The library returns the
negated errno value from the call, and `xnvme_fs_errmsg()` returns the
message.

The server closes a connection that sends a frame longer than 64 MiB or that
ends inside a frame. The server answers with `EPROTO` to a request that has
too few bytes for its arguments, and to an unknown operation code.

## Operations

The first request on a connection must be `HELLO`; any other request before
`HELLO` fails with `EPROTO`. `lib/protocol.hpp` declares the codes in
`enum class Op`, and declares `class Client`, which sends every operation.

| Code | Operation | Arguments | Results |
|--|--|--|--|
| 1 | `HELLO` | protocol version (32 bits, 1), pool name | extent size, data offset, member names (a 32-bit count and a string each) |
| 2 | `OPEN` | name, flags (32 bits: 1 create, 2 truncate, 4 exclusive) | handle, size, extents |
| 3 | `CLOSE` | handle, size | none |
| 4 | `LEASE` | handle, count (32 bits, 1 to 4096) | the new extents |
| 5 | `COMMIT` | handle, size | none |
| 6 | `TRUNCATE` | handle, size | none |
| 7 | `STAT` | name | kind (8 bits: 0 none, 1 file, 2 directory), size |
| 8 | `LIST` | directory, offset, maximum count (32 bits) | version of the names, entries remaining after these, entries (a 32-bit count, then name, kind and size each) |
| 9 | `MKDIR` | name | none |
| 10 | `RMDIR` | name | none |
| 11 | `REMOVE` | name | none |
| 12 | `RENAME` | name, new name | none |
| 13 | `INFO` | none | pool name, layout version (32 bits), extent size, generation, number of files, members (a 32-bit count, then name, size, extents and free extents each) |
| 14 | `INDEX` | none | lease, version, extent size, data offset, members (a 32-bit count, then name, md chunk size, and a 32-bit count of drives with a name and a start each), directories (a 32-bit count and a string each), files (a 32-bit count, then name, size and extents each) |
| 15 | `WATCH` | lease | the version that made the index stale |
| 16 | `RELEASE` | lease | none |

Fields without a stated width are 64 bits. Handles, sizes and offsets are
64-bit integers; extents are 64-bit [addresses](extents.md#extent-addresses).

`HELLO` fails with `EPROTO` for another protocol version and with `ENOENT`
when the server serves another pool. A client computes the location of file
data on the members from the results of `HELLO` and the map of a file.

`LIST` takes the empty string as the name of the top directory. `LIST` returns
the entries of one directory sorted by name in byte order, at most 4096 per
request (`MAX_LIST`), and fails with `ENOENT` for a name that is not a
directory. The version in the results changes whenever a name in the pool is
added, removed or renamed. A client that pages through a directory compares
the versions of two answers to detect that the names changed between the two
requests.

`INFO` counts every file in the server's namespace: the files in the table and
the files never committed. The generation is the generation of the last table
written.

[Leases and commits](leases.md) describes what `OPEN`, `LEASE`, `COMMIT`,
`TRUNCATE`, `CLOSE`, `REMOVE`, `RENAME`, `MKDIR`, `RMDIR` and the read leases
`INDEX`, `WATCH` and `RELEASE` do, and the errors they fail with.

## Deferred answers to WATCH

`WATCH` is the only request that the server may answer later. When the index
of the lease is not yet stale, the server records the watch and continues to
handle the requests of other connections. The server sends the answer after
the request that makes the index stale. The client waits for the answer on its
connection and cannot send another request on that connection until the answer
arrives. `WATCH` and `RELEASE` fail with `EINVAL` on any connection other than
the connection that took the lease. A reader therefore uses a separate
connection to take, watch and release its index.
