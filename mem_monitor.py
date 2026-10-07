#!/usr/bin/env python3
import argparse
import csv
import json
import os
import re
import shlex
import signal
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import List, Optional

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


STATUS_KEYS = ("VmRSS", "VmHWM", "VmSize")

DEFAULT_MARKERS = [
    # (r"Target System:", "target_system"),
    (r"Finish Graph Data Loading...", "input_loaded"),
    (r"Finish System Initialization...", "initialized"),
    (r"Starting Concurrent Write Benchmark", "concurrent_write_start"),
    # (r"Starting Write Benchmark", "write_start"),
    (r"Graph loading done.", "concurrent_rw_start"),
    # (r"Inserted \d+ edges", "insert_finished"),
    (r"Concurrent Benchmark Total Time", "concurrent_rw_done"),
    (r"Starting PageRank", "pagerank_start"),
    (r"Starting BFS", "bfs_start"),
    (r"Starting SSSP", "sssp_start"),
    # Experiment 1: Mixed-update.
    (r"Mixed Load Start", "mixed_load_start"),
    (r"Mixed Update Start", "mixed_start"),
    (r"Mixed Update Done", "mixed_done"),
    # Experiment 2: HotSet update.
    (r"HotSet Load Start", "hotset_load_start"),
    (r"HotSet Update Start", "hotset_update_start"),
    (r"HotSet Update Done", "hotset_done"),
    (r"\[STALL\]", "stall"),
]


@dataclass
class Event:
    time_sec: float
    marker: str
    line: str


def parse_status_kb(pid: int):
    status_path = Path(f"/proc/{pid}/status")
    result = {"VmRSS": 0, "VmHWM": 0, "VmSize": 0}
    try:
        with status_path.open("r", encoding="utf-8", errors="replace") as f:
            for line in f:
                for key in STATUS_KEYS:
                    if line.startswith(key + ":"):
                        parts = line.split()
                        if len(parts) >= 2 and parts[1].isdigit():
                            result[key] = int(parts[1])
    except FileNotFoundError:
        pass
    return result


def kb_to_mb(v: int) -> float:
    return v / 1024.0


def build_parser():
    parser = argparse.ArgumentParser(
        description="Non-intrusive RSS/HWM sampler for your graph benchmark."
    )
    parser.add_argument(
        "--interval",
        type=float,
        default=0.2,
        help="Sampling interval in seconds. Default: 0.2",
    )
    parser.add_argument(
        "--out-prefix",
        required=True,
        help="Output prefix, e.g. results/avb_pr_32r32w",
    )
    parser.add_argument(
        "--label",
        default="",
        help="Optional label written into the summary JSON.",
    )
    parser.add_argument(
        "--stdout-log",
        default="",
        help="Optional explicit stdout log file path. Defaults to <out-prefix>.stdout.log",
    )
    parser.add_argument(
        "--extra-marker",
        action="append",
        default=[],
        help='Extra regex markers in the form "regex=>name". Can be used multiple times.',
    )
    parser.add_argument(
        "cmd",
        nargs=argparse.REMAINDER,
        help="Command to run. Use -- before the command, e.g. -- ./bench ...",
    )
    return parser


def strip_remainder_prefix(cmd: List[str]) -> List[str]:
    if cmd and cmd[0] == "--":
        return cmd[1:]
    return cmd


def read_stream(proc, start_time: float, stdout_log_path: Path, events: List[Event], marker_patterns):
    with stdout_log_path.open("w", encoding="utf-8") as logf:
        for raw_line in proc.stdout:
            line = raw_line.rstrip("\n")
            now = time.time() - start_time
            logf.write(raw_line)
            logf.flush()
            print(line, flush=True)
            for pattern, name in marker_patterns:
                if pattern.search(line):
                    events.append(Event(time_sec=now, marker=name, line=line))
                    break


def write_csv(samples, csv_path: Path):
    with csv_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(
            ["time_sec", "rss_kb", "rss_mb", "hwm_kb", "hwm_mb", "vmsize_kb", "vmsize_mb", "last_marker"]
        )
        for row in samples:
            writer.writerow(
                [
                    f"{row['time_sec']:.6f}",
                    row["rss_kb"],
                    f"{kb_to_mb(row['rss_kb']):.6f}",
                    row["hwm_kb"],
                    f"{kb_to_mb(row['hwm_kb']):.6f}",
                    row["vmsize_kb"],
                    f"{kb_to_mb(row['vmsize_kb']):.6f}",
                    row["last_marker"],
                ]
            )


def write_events(events: List[Event], events_path: Path):
    with events_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(["time_sec", "marker", "line"])
        for e in events:
            writer.writerow([f"{e.time_sec:.6f}", e.marker, e.line])


def plot_curve(samples, events: List[Event], png_path: Path, title: str):
    if not samples:
        return
    x = [row["time_sec"] for row in samples]
    rss = [kb_to_mb(row["rss_kb"]) for row in samples]
    hwm = [kb_to_mb(row["hwm_kb"]) for row in samples]

    fig, ax = plt.subplots(figsize=(10, 5))
    ax.plot(x, rss, label="RSS (MB)")
    ax.plot(x, hwm, label="HWM (MB)")
    for e in events:
        ax.axvline(e.time_sec, linestyle="--", linewidth=0.8)
        ax.text(
            e.time_sec,
            max(hwm) if hwm else 0.0,
            e.marker,
            rotation=90,
            va="top",
            ha="right",
            fontsize=8,
        )
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Memory (MB)")
    ax.set_title(title)
    ax.legend()
    ax.grid(True, alpha=0.25)
    fig.tight_layout()
    fig.savefig(png_path, dpi=160)
    plt.close(fig)


def main():
    parser = build_parser()
    args = parser.parse_args()
    cmd = strip_remainder_prefix(args.cmd)

    if not cmd:
        parser.error("No benchmark command was provided. Use -- before the command.")

    out_prefix = Path(args.out_prefix)
    out_prefix.parent.mkdir(parents=True, exist_ok=True)

    stdout_log_path = Path(args.stdout_log) if args.stdout_log else out_prefix.with_suffix(".stdout.log")
    csv_path = out_prefix.with_suffix(".mem.csv")
    events_path = out_prefix.with_suffix(".events.csv")
    png_path = out_prefix.with_suffix(".png")
    summary_path = out_prefix.with_suffix(".summary.json")

    marker_patterns = [(re.compile(p), name) for p, name in DEFAULT_MARKERS]
    for item in args.extra_marker:
        if "=>" not in item:
            raise SystemExit(f'Bad --extra-marker: "{item}". Expected "regex=>name".')
        regex, name = item.split("=>", 1)
        marker_patterns.append((re.compile(regex), name))

    print(">>> Launch command:")
    print(" ".join(shlex.quote(x) for x in cmd))
    print(f">>> Sampling interval: {args.interval}s")
    print(f">>> Output prefix: {out_prefix}")

    start_wall = time.time()
    events: List[Event] = []
    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
        preexec_fn=os.setsid,
    )

    reader = threading.Thread(
        target=read_stream,
        args=(proc, start_wall, stdout_log_path, events, marker_patterns),
        daemon=True,
    )
    reader.start()

    samples = []
    last_marker = ""
    return_code: Optional[int] = None

    try:
        while True:
            rc = proc.poll()
            for e in events:
                last_marker = e.marker
            stat = parse_status_kb(proc.pid)
            samples.append(
                {
                    "time_sec": time.time() - start_wall,
                    "rss_kb": stat["VmRSS"],
                    "hwm_kb": stat["VmHWM"],
                    "vmsize_kb": stat["VmSize"],
                    "last_marker": last_marker,
                }
            )
            if rc is not None:
                return_code = rc
                break
            time.sleep(args.interval)
    except KeyboardInterrupt:
        print(">>> Interrupted, terminating benchmark...")
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
        except ProcessLookupError:
            pass
        return_code = proc.wait()
    finally:
        reader.join(timeout=2.0)
        stat = parse_status_kb(proc.pid)
        samples.append(
            {
                "time_sec": time.time() - start_wall,
                "rss_kb": stat["VmRSS"],
                "hwm_kb": stat["VmHWM"],
                "vmsize_kb": stat["VmSize"],
                "last_marker": last_marker or "process_exit",
            }
        )

    write_csv(samples, csv_path)
    write_events(events, events_path)
    title = args.label or out_prefix.name
    plot_curve(samples, events, png_path, title)

    peak_rss_kb = max((row["rss_kb"] for row in samples), default=0)
    peak_hwm_kb = max((row["hwm_kb"] for row in samples), default=0)

    summary = {
        "label": args.label,
        "cmd": cmd,
        "return_code": return_code,
        "duration_sec": samples[-1]["time_sec"] if samples else 0.0,
        "peak_rss_kb": peak_rss_kb,
        "peak_rss_mb": kb_to_mb(peak_rss_kb),
        "peak_hwm_kb": peak_hwm_kb,
        "peak_hwm_mb": kb_to_mb(peak_hwm_kb),
        "samples": len(samples),
        "events": [asdict(e) for e in events],
        "files": {
            "csv": str(csv_path),
            "events_csv": str(events_path),
            "stdout_log": str(stdout_log_path),
            "plot_png": str(png_path),
        },
    }
    with summary_path.open("w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2, ensure_ascii=False)

    print(">>> Done.")
    print(json.dumps(summary, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
