#!/usr/bin/env python3
"""Statistics from a Ryujinx trace log (SVCs, IPC commands, threads, memory).

Expects a log written by Ryujinx (Ryubing 1.3.x) with Trace, Debug and Stub
logging enabled. Relevant line shapes:

    00:00:07.434 |T| <MainThread> KernelSvc : CreateThread(entrypoint: 0x.., priority: 0x2C, cpuCore: 0x1)
    00:00:07.434 |T| <MainThread> KernelSvc : CreateThread(handle: 0x00068008) = Success
    00:00:07.577 |T| HLE.OsThread.2 KernelIpc CallCmifMethod: IFileSystemProxy: SetCurrentProcess
    00:00:01.786 |T| HLE.OsThread.18 KernelIpc ProcessMessageImpl: LogService.OpenLogger called
    00:00:07.771 |S| HLE.OsThread.4 ServiceHid SetSupportedNpadStyleSet: Stubbed. ...

The raw log stays in $LGP_PRIVATE_DIR. The report contains only call names,
counts, thread names/priorities and memory sizes, no code or assets.

Usage:
    python tools/analyze/ryujinx_log_stats.py <log> [--json out.json] [--markdown out.md]
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter
from dataclasses import dataclass, field

LINE_RE = re.compile(
    r"^(?P<time>\d+:\d+:\d+\.\d+) \|(?P<level>\w)\| "
    r"(?P<thread><[^>]*>|\S+) (?P<cls>\w+) ?(?P<caller>[\w.]*): (?P<msg>.*)$"
)
SVC_CALL_RE = re.compile(r"^(?P<name>\w+)\((?P<args>.*)\)$")
SVC_RET_RE = re.compile(r"^(?P<name>\w+)\((?P<args>.*)\) = (?P<result>\w+)$")
ARG_RE = re.compile(r"(\w+): (0x[0-9A-Fa-f]+|\w+)")
CMIF_RE = re.compile(r"^(?P<iface>\w+): (?P<cmd>\w+)$")
HORIZON_RE = re.compile(r"^(?P<iface>[\w.]+)\.(?P<cmd>\w+) called$")
NVMAP_CREATE_RE = re.compile(r"^Created map (?P<id>\d+) with size (?P<size>0x[0-9A-Fa-f]+)!$")
NVMAP_DELETE_RE = re.compile(r"^Deleted map (?P<id>\d+)!$")
APPLET_RE = re.compile(r"^Applet '(?P<name>\w+)' created\.$")

GIB = 1 << 30
MIB = 1 << 20


def parse_args(text: str) -> dict[str, str]:
    return {k: v for k, v in ARG_RE.findall(text)}


def as_int(value: str | None) -> int | None:
    if value is None:
        return None
    try:
        return int(value, 16) if value.startswith("0x") else int(value)
    except ValueError:
        return None


def is_guest_thread(thread: str) -> bool:
    """Guest threads appear as <Name> once named, before that as HLE.GuestThread.N.

    HLE.OsThread.* are Ryujinx host service threads; other tokens (Gpu, Ptc, ...)
    are host log sources without a thread column.
    """
    return thread.startswith("<") or thread.startswith("HLE.GuestThread.")


def signed32(value: int | None) -> int | None:
    if value is None:
        return None
    return value - (1 << 32) if value >= 1 << 31 else value


def to_seconds(t: str) -> float:
    h, m, s = t.split(":")
    return int(h) * 3600 + int(m) * 60 + float(s)


@dataclass
class ThreadInfo:
    name: str
    first_seen: float
    svc_calls: int = 0


@dataclass
class Stats:
    lines: int = 0
    parsed: int = 0
    svc_calls: Counter = field(default_factory=Counter)
    svc_results: Counter = field(default_factory=Counter)  # (name, result) for result != Success
    ipc_cmds: Counter = field(default_factory=Counter)  # "Iface.Cmd"
    stubs: Counter = field(default_factory=Counter)
    warnings: Counter = field(default_factory=Counter)
    errors: Counter = field(default_factory=Counter)
    threads: dict[str, ThreadInfo] = field(default_factory=dict)
    created_threads: list[dict] = field(default_factory=list)
    heap_sizes: list[int] = field(default_factory=list)
    total_memory_size: int | None = None
    used_memory_max: int = 0
    phys_mapped: int = 0
    phys_mapped_peak: int = 0
    transfer_mem: list[int] = field(default_factory=list)
    # nvmap handles (GPU-visible buffers carved from guest memory): id -> size.
    nvmap_live: dict[int, int] = field(default_factory=dict)
    nvmap_live_bytes: int = 0
    nvmap_peak_bytes: int = 0
    nvmap_created: int = 0
    applets: Counter = field(default_factory=Counter)
    # Per-thread pending SVC (name, args) to pair the "= Result" line with its call.
    _pending: dict[str, tuple[str, dict[str, str]]] = field(default_factory=dict)

    def feed(self, line: str) -> None:
        self.lines += 1
        m = LINE_RE.match(line.rstrip("\r\n"))
        if not m:
            return
        self.parsed += 1
        level, thread, cls, caller, msg = (m["level"], m["thread"], m["cls"], m["caller"], m["msg"])
        if is_guest_thread(thread) and thread not in self.threads:
            self.threads[thread] = ThreadInfo(thread, to_seconds(m["time"]))

        if cls == "KernelSvc":
            self._svc(thread, msg)
        elif cls == "KernelIpc":
            self._ipc(caller, msg)
        elif cls == "ServiceNv" and caller in ("Create", "DecrementMapRefCount"):
            self._nvmap(msg)
        elif cls == "ServiceAm" and (am := APPLET_RE.match(msg)):
            self.applets[am["name"]] += 1
        elif level == "S":
            self.stubs[f"{cls}.{caller}"] += 1
        elif level == "W":
            self.warnings[f"{cls}.{caller}"] += 1
        elif level == "E":
            self.errors[f"{cls}.{caller}"] += 1

    def _nvmap(self, msg: str) -> None:
        m = NVMAP_CREATE_RE.match(msg)
        if m:
            handle, size = int(m["id"]), int(m["size"], 16)
            self.nvmap_live_bytes += size - self.nvmap_live.get(handle, 0)
            self.nvmap_live[handle] = size
            self.nvmap_created += 1
            self.nvmap_peak_bytes = max(self.nvmap_peak_bytes, self.nvmap_live_bytes)
            return
        m = NVMAP_DELETE_RE.match(msg)
        if m:
            self.nvmap_live_bytes -= self.nvmap_live.pop(int(m["id"]), 0)

    def _ipc(self, caller: str, msg: str) -> None:
        if caller in ("CallCmifMethod", "CallTipcMethod"):
            m = CMIF_RE.match(msg)
            if m:
                self.ipc_cmds[f"{m['iface']}.{m['cmd']}"] += 1
        elif caller == "ProcessMessageImpl":
            m = HORIZON_RE.match(msg)
            if m:
                self.ipc_cmds[f"{m['iface']}.{m['cmd']}"] += 1

    def _svc(self, thread: str, msg: str) -> None:
        ret = SVC_RET_RE.match(msg)
        if ret:
            name, result = ret["name"], ret["result"]
            call = self._pending.pop(thread, None)
            if result.startswith("0x"):
                # Plain return value (e.g. GetCurrentProcessorNumber), not a result code.
                return
            if result != "Success":
                self.svc_results[(name, result)] += 1
            if call is None or call[0] != name or result != "Success":
                return
            out = parse_args(ret["args"])
            self._svc_result(name, call[1], out)
            return
        call = SVC_CALL_RE.match(msg)
        if not call:
            return
        name, args = call["name"], parse_args(call["args"])
        pending = self._pending.get(thread)
        if pending and pending[0] == name and not call["args"]:
            # Void SVCs (SleepThread, ExitThread, ...) log their return as "Name()".
            del self._pending[thread]
            return
        self.svc_calls[name] += 1
        if thread in self.threads:
            self.threads[thread].svc_calls += 1
        self._pending[thread] = (name, args)

    def _svc_result(self, name: str, args: dict[str, str], out: dict[str, str]) -> None:
        if name == "SetHeapSize":
            size = as_int(args.get("size"))
            if size is not None:
                self.heap_sizes.append(size)
        elif name == "GetInfo":
            value = as_int(out.get("value"))
            if value is None:
                return
            if args.get("id") == "TotalMemorySize":
                self.total_memory_size = value
            elif args.get("id") == "UsedMemorySize":
                self.used_memory_max = max(self.used_memory_max, value)
        elif name == "MapPhysicalMemory":
            self.phys_mapped += as_int(args.get("size")) or 0
            self.phys_mapped_peak = max(self.phys_mapped_peak, self.phys_mapped)
        elif name == "UnmapPhysicalMemory":
            self.phys_mapped -= as_int(args.get("size")) or 0
        elif name == "CreateTransferMemory":
            size = as_int(args.get("size"))
            if size is not None:
                self.transfer_mem.append(size)
        elif name == "CreateThread":
            self.created_threads.append({
                "priority": as_int(args.get("priority")),
                "cpu_core": signed32(as_int(args.get("cpuCore"))),
                "handle": out.get("handle"),
            })

    def to_dict(self) -> dict:
        prios = Counter(t["priority"] for t in self.created_threads)
        cores = Counter(t["cpu_core"] for t in self.created_threads)
        return {
            "lines": self.lines,
            "parsed_lines": self.parsed,
            "svc_calls": dict(self.svc_calls.most_common()),
            "svc_non_success": {f"{n}={r}": c for (n, r), c in self.svc_results.most_common()},
            "ipc_commands": dict(self.ipc_cmds.most_common()),
            "ipc_interfaces": dict(self.interfaces().most_common()),
            "stubs": dict(self.stubs.most_common()),
            "warnings": dict(self.warnings.most_common()),
            "errors": dict(self.errors.most_common()),
            "threads": [
                {"name": t.name, "first_seen_s": round(t.first_seen, 3), "svc_calls": t.svc_calls}
                for t in sorted(self.threads.values(), key=lambda t: t.first_seen)
            ],
            "created_threads": len(self.created_threads),
            "created_thread_priorities": {str(k): v for k, v in sorted(prios.items(), key=lambda kv: (kv[0] is None, kv[0]))},
            "created_thread_cores": {str(k): v for k, v in sorted(cores.items(), key=lambda kv: (kv[0] is None, kv[0]))},
            "memory": {
                "heap_sizes": self.heap_sizes,
                "heap_size_max": max(self.heap_sizes, default=0),
                "total_memory_size": self.total_memory_size,
                "used_memory_size_max": self.used_memory_max,
                "physical_mapped_peak": self.phys_mapped_peak,
                "transfer_memory_sizes": self.transfer_mem,
                "nvmap_created": self.nvmap_created,
                "nvmap_peak_bytes": self.nvmap_peak_bytes,
                "nvmap_live_bytes_at_end": self.nvmap_live_bytes,
            },
            "applets": dict(self.applets.most_common()),
        }

    def interfaces(self) -> Counter:
        c: Counter = Counter()
        for key, n in self.ipc_cmds.items():
            c[key.rsplit(".", 1)[0]] += n
        return c


def fmt_size(n: int | None) -> str:
    if not n:
        return "–"
    if n >= GIB:
        return f"{n / GIB:.2f} GiB (0x{n:X})"
    return f"{n / MIB:.1f} MiB (0x{n:X})"


def to_markdown(d: dict, top: int = 0) -> str:
    def table(title: str, items: dict, head: str) -> list[str]:
        rows = list(items.items())
        if top:
            rows = rows[:top]
        out = [f"### {title} ({len(items)})", "", f"| {head} | count |", "|---|---:|"]
        out += [f"| `{k}` | {v} |" for k, v in rows]
        return out + [""]

    mem = d["memory"]
    lines = [
        f"lines: {d['lines']} (of these recognised: {d['parsed_lines']})",
        "",
        "### Memory",
        "",
        f"- SetHeapSize max: {fmt_size(mem['heap_size_max'])} (calls: {len(mem['heap_sizes'])})",
        f"- GetInfo TotalMemorySize: {fmt_size(mem['total_memory_size'])}",
        f"- GetInfo UsedMemorySize max: {fmt_size(mem['used_memory_size_max'])}",
        f"- MapPhysicalMemory peak: {fmt_size(mem['physical_mapped_peak'])}",
        f"- TransferMemory: {len(mem['transfer_memory_sizes'])} items, total {fmt_size(sum(mem['transfer_memory_sizes']))}",
        f"- nvmap: {mem['nvmap_created']} handles created, peak at once {fmt_size(mem['nvmap_peak_bytes'])}, at the end {fmt_size(mem['nvmap_live_bytes_at_end'])}",
        "",
        "Applets: " + (", ".join(f"{k}×{v}" for k, v in d["applets"].items()) or "–"),
        "",
        f"### Threads: {d['created_threads']} per CreateThread, {len(d['threads'])} guest threads in the log",
        "",
        "priorities (CreateThread): " + ", ".join(f"{k}×{v}" for k, v in d["created_thread_priorities"].items()),
        "",
        "cores (cpuCore): " + ", ".join(f"{k}×{v}" for k, v in d["created_thread_cores"].items()),
        "",
    ]
    lines += table("SVCs", d["svc_calls"], "SVC")
    lines += table("SVC results ≠ Success", d["svc_non_success"], "SVC=result")
    lines += table("IPC interfaces", d["ipc_interfaces"], "interface")
    lines += table("IPC commands", d["ipc_commands"], "command")
    lines += table("Stubs", d["stubs"], "Stub")
    return "\n".join(lines)


def analyze(stream) -> Stats:
    stats = Stats()
    for line in stream:
        stats.feed(line)
    return stats


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Statistics from a Ryujinx trace log.")
    ap.add_argument("log")
    ap.add_argument("--json")
    ap.add_argument("--markdown")
    ap.add_argument("--top", type=int, default=0, help="limit table rows (0 = all)")
    a = ap.parse_args(argv)
    with open(a.log, encoding="utf-8", errors="replace") as f:
        d = analyze(f).to_dict()
    md = to_markdown(d, a.top)
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(d, f, indent=2)
    if a.markdown:
        with open(a.markdown, "w", encoding="utf-8") as f:
            f.write(md)
    if not a.json and not a.markdown:
        sys.stdout.write(md + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
