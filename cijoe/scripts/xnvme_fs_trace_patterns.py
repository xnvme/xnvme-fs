# SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
# SPDX-License-Identifier: BSD-3-Clause
"""
xnvme_fs_trace_patterns: the I/O patterns in the traces the trace suite left
============================================================================

Fetches duckdb.trace and rocksdb.trace from xnvme_fs.trace_dir on the target and writes, per engine and kind of
file, these measures of its I/O patterns, to artifacts/io-patterns.md and .json:

  share     the kind's bytes read and written, of the engine's
  r:w       bytes read to bytes written
  request   median read and write sizes
  appends   writes starting at or past the file's end as the trace has it: the highest offset written to it
            before, lowered by a truncate and forgotten at a removal
  seq       reads starting where the previous read of the file ended
  raw       bytes read from files the trace had written to before
  per sync  bytes written to the kind per sync of it

Retargetable: False
-------------------
"""
import errno
import json
import logging as log
from collections import defaultdict
from pathlib import Path
from statistics import median

READS = ("read", "submit_read")
WRITES = ("write", "submit_write")


def kind(engine, name):
    base = name.rsplit("/", 1)[-1]
    if engine == "rocksdb":
        if base.endswith(".log"):
            return "write-ahead log"
        if base == "LOG" or base.startswith("LOG.old"):
            return "info log"
        if base.endswith(".sst"):
            return "sorted tables"
        if base.startswith(("MANIFEST", "CURRENT", "OPTIONS", "IDENTITY", "LOCK")) or base.endswith(".dbtmp"):
            return "metadata"
        return "other"
    if base.endswith((".wal", ".wal.checkpoint")):
        return "write-ahead log"
    if ".tmp" in name or "duckdb_temp" in name:
        return "temporary"
    if base.endswith(".db"):
        return "database"
    return "other"


def analyse(engine, lines):
    """Measures per kind of file from the lines of one engine's trace."""
    k = defaultdict(lambda: defaultdict(float))
    sizes = defaultdict(lambda: {"read": [], "write": []})
    top = {}
    last_read = {}
    written = set()
    for line in lines:
        f = line.split()
        if len(f) < 7:
            continue
        op, name, offset, nbytes = f[3], f[4], int(f[5]), int(f[6])
        if op == "rename":
            src, _, dst = name.partition(">")
            for table in (top, last_read):
                if src in table:
                    table[dst] = table.pop(src)
            if src in written:
                written.discard(src)
                written.add(dst)
            continue
        if op == "truncate":
            top[name] = min(top.get(name, 0), offset)
            continue
        if op == "remove":
            top.pop(name, None)
            last_read.pop(name, None)
            written.discard(name)
            continue
        if op not in READS + WRITES + ("sync",):
            continue
        c = k[kind(engine, name)]
        if op in WRITES:
            c["written"] += nbytes
            c["writes"] += 1
            if offset >= top.get(name, 0):
                c["appends"] += 1
            top[name] = max(top.get(name, 0), offset + nbytes)
            written.add(name)
            sizes[kind(engine, name)]["write"].append(nbytes)
        elif op in READS:
            c["read"] += nbytes
            c["reads"] += 1
            if last_read.get(name) == offset:
                c["sequential"] += 1
            last_read[name] = offset + nbytes
            if name in written:
                c["read_after_write"] += nbytes
            sizes[kind(engine, name)]["read"].append(nbytes)
        elif op == "sync":
            c["syncs"] += 1

    total = sum(c["read"] + c["written"] for c in k.values()) or 1
    rows = {}
    for name, c in sorted(k.items()):
        r, w = c["read"], c["written"]
        rows[name] = {
            "share": (r + w) / total,
            "read_bytes": r,
            "write_bytes": w,
            "reads_per_write": r / w if w else None,
            "median_read": median(sizes[name]["read"]) if sizes[name]["read"] else None,
            "median_write": median(sizes[name]["write"]) if sizes[name]["write"] else None,
            "appends": c["appends"] / c["writes"] if c["writes"] else None,
            "sequential_reads": c["sequential"] / c["reads"] if c["reads"] else None,
            "read_after_write": c["read_after_write"] / r if r else None,
            "bytes_per_sync": w / c["syncs"] if c["syncs"] else None,
            "syncs": int(c["syncs"]),
        }
    return rows


def human(n):
    if n is None:
        return "-"
    for unit in ("B", "KiB", "MiB", "GiB"):
        if abs(n) < 1024 or unit == "GiB":
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1024
    return str(n)


def pct(x):
    if x is None:
        return "-"
    return f"{100 * x:.1f}%" if x < 0.01 else f"{100 * x:.0f}%"


def ratio(x):
    return "-" if x is None else f"{x:.2g}"


def table(results):
    out = [
        "| Engine, file | Share of bytes | Reads : writes | Median read | Median write | Appends | Sequential reads"
        " | Read after write | Bytes per sync |",
        "|--|--:|--:|--:|--:|--:|--:|--:|--:|",
    ]
    for engine, rows in results.items():
        for name, r in rows.items():
            out.append(
                f"| {engine}, {name} | {pct(r['share'])} | {ratio(r['reads_per_write'])} | {human(r['median_read'])}"
                f" | {human(r['median_write'])} | {pct(r['appends'])} | {pct(r['sequential_reads'])}"
                f" | {pct(r['read_after_write'])} | {human(r['bytes_per_sync'])} |"
            )
    return "\n".join(out) + "\n"


def main(args, cijoe):
    trace_dir = cijoe.getconf("xnvme_fs.trace_dir", "/var/tmp/xnvme-fs-trace")
    artifacts = Path(cijoe.output_path) / "artifacts"
    artifacts.mkdir(parents=True, exist_ok=True)
    results = {}
    for engine in ("duckdb", "rocksdb"):
        local = artifacts / f"{engine}.trace"
        if cijoe.get(f"{trace_dir}/{engine}.trace", str(local)) is False or not local.exists():
            log.error(f"no {engine}.trace in {trace_dir}")
            return errno.ENOENT
        results[engine] = analyse(engine, local.read_text().splitlines())
    (artifacts / "io-patterns.json").write_text(json.dumps(results, indent=2) + "\n")
    (artifacts / "io-patterns.md").write_text(table(results))
    log.info("\n" + table(results))
    return 0
