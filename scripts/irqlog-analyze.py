#!/usr/bin/env python3
"""Helper to annotate qemu irq-log output with thread and symbol info.

This parses the multiline irq-log records emitted by qemu_set_irq() and adds
two conveniences:

* Resolve the host thread id (TID) to a friendly thread name using /proc.
* Resolve the recorded return address to a function + source location using
  addr2line (or a compatible tool).

Example:

  $ ./scripts/irqlog-analyze.py --pid $(pgrep qemu-system-x86_64) \
        --binary build/qemu-system-x86_64 irq.log

The script expects logs captured with the multi-line format introduced for the
IRQ logger (time/level/kind line followed by path/irq handler lines).  The
output is a compact summary per interrupt event.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional


HEADER_RE = re.compile(
    r"irq-log:\s+time=(?P<time>\d+)ns\s+level=(?P<level>-?\d+)\s+"
    r"n=(?P<n>-?\d+)\s+kind=(?P<kind>.+)"
)
PATH_RE = re.compile(r"^\s*path=(?P<path>.+)")
IRQ_LINE_RE = re.compile(
    r"^\s*irq=(?P<irq>0x[0-9a-fA-F]+)\s+handler=(?P<handler>0x[0-9a-fA-F]+)\s+"
    r"opaque=(?P<opaque>[^\s]+)"
)
TAIL_RE = re.compile(
    r"host-tid=(?P<tid>\d+)\s+caller=(?P<caller>0x[0-9a-fA-F]+)"
)


@dataclass
class IrqRecord:
    raw: List[str]
    time_ns: Optional[int] = None
    level: Optional[int] = None
    line: Optional[int] = None
    kind: Optional[str] = None
    path: Optional[str] = None
    irq_ptr: Optional[str] = None
    handler_ptr: Optional[str] = None
    opaque: Optional[str] = None
    tid: Optional[int] = None
    caller: Optional[str] = None


def parse_records(lines: Iterable[str]) -> List[IrqRecord]:
    records: List[IrqRecord] = []
    current: Optional[IrqRecord] = None

    for line in lines:
        stripped = line.rstrip("\n")
        if "irq-log:" in stripped:
            if current is not None and current.raw:
                records.append(current)
            current = IrqRecord(raw=[])

        if current is None:
            continue

        current.raw.append(stripped)

        header = HEADER_RE.search(stripped)
        if header:
            current.time_ns = int(header.group("time"))
            current.level = int(header.group("level"))
            current.line = int(header.group("n"))
            current.kind = header.group("kind").strip()

        path_m = PATH_RE.search(stripped)
        if path_m:
            current.path = path_m.group("path")

        irq_m = IRQ_LINE_RE.search(stripped)
        if irq_m:
            current.irq_ptr = irq_m.group("irq")
            current.handler_ptr = irq_m.group("handler")
            current.opaque = irq_m.group("opaque")

        tail_m = TAIL_RE.search(stripped)
        if tail_m:
            current.tid = int(tail_m.group("tid"))
            current.caller = tail_m.group("caller")

    if current is not None and current.raw:
        records.append(current)

    return records


def read_thread_name(pid: int, tid: int, cache: Dict[int, str]) -> Optional[str]:
    if tid in cache:
        return cache[tid]

    path = Path("/proc") / str(pid) / "task" / str(tid) / "comm"
    try:
        name = path.read_text().strip()
        cache[tid] = name
        return name
    except OSError:
        cache[tid] = ""
        return None


def resolve_symbol(binary: Path, addr: str, cache: Dict[str, str]) -> Optional[str]:
    if addr in cache:
        return cache[addr]

    if not binary:
        cache[addr] = ""
        return None

    cmd = [
        os.environ.get("ADDR2LINE", "addr2line"),
        "-Cfpe",
        str(binary),
        addr,
    ]
    try:
        out = subprocess.check_output(cmd, stderr=subprocess.STDOUT)
    except (OSError, subprocess.CalledProcessError) as exc:
        cache[addr] = ""
        sys.stderr.write(f"[irqlog-analyze] addr2line failed for {addr}: {exc}\n")
        return None

    decoded = out.decode().strip()
    cache[addr] = decoded
    return decoded


def format_record(record: IrqRecord,
                  thread_name: Optional[str],
                  symbol: Optional[str]) -> str:
    parts = []
    header = (
        f"time={record.time_ns}ns"
        if record.time_ns is not None else "time=?"
    )
    header += f" level={record.level if record.level is not None else '?'}"
    header += f" n={record.line if record.line is not None else '?'}"
    header += f" kind={record.kind or '?'}"
    parts.append(header)

    if record.path:
        parts.append(f"path={record.path}")

    if record.irq_ptr or record.handler_ptr or record.opaque:
        frag = []
        if record.irq_ptr:
            frag.append(f"irq={record.irq_ptr}")
        if record.handler_ptr:
            frag.append(f"handler={record.handler_ptr}")
        if record.opaque:
            frag.append(f"opaque={record.opaque}")
        parts.append(" ".join(frag))

    if record.tid is not None:
        label = f"tid={record.tid}"
        if thread_name:
            label += f" ({thread_name})"
        parts.append(label)

    if record.caller:
        entry = f"caller={record.caller}"
        if symbol:
            entry += f" -> {symbol}"
        parts.append(entry)

    body = "\n  ".join(parts)
    raw = "\n    ".join(record.raw)
    return f"- {body}\n    raw: {raw}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logfile", nargs="?", type=Path, default=None,
                        help="Path to irq-log output (defaults to stdin)")
    parser.add_argument("--pid", type=int,
                        help="QEMU process id to resolve TID -> thread name")
    parser.add_argument("--binary", type=Path,
                        help="QEMU binary with symbols for addr2line lookups")
    args = parser.parse_args()

    if args.logfile:
        try:
            data = args.logfile.read_text().splitlines()
        except OSError as exc:
            sys.stderr.write(f"Failed to read {args.logfile}: {exc}\n")
            return 1
    else:
        data = [line.rstrip("\n") for line in sys.stdin]

    records = parse_records(data)
    if not records:
        sys.stderr.write("[irqlog-analyze] no irq-log records detected\n")
        return 1

    tid_cache: Dict[int, str] = {}
    sym_cache: Dict[str, str] = {}

    for rec in records:
        thread_name = None
        if args.pid and rec.tid is not None:
            thread_name = read_thread_name(args.pid, rec.tid, tid_cache)

        symbol = None
        if args.binary and rec.caller:
            symbol = resolve_symbol(args.binary, rec.caller, sym_cache)

        print(format_record(rec, thread_name, symbol))

    return 0


if __name__ == "__main__":
    sys.exit(main())
