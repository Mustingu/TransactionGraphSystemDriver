#!/usr/bin/env python3
import argparse
from pathlib import Path
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt

MODE_CONFIG = {
    "concurrent": {
        "baseline_marker": "initialized",
        "phase1_start_marker": "initialized",
        "phase1_end_marker": "concurrent_rw_start",
        "phase2_start_marker": "concurrent_rw_start",
        "phase2_end_marker": "concurrent_rw_done",
        "phase1_label": "Insert",
        "phase2_label": "Concurrent RW",
    },
    "hotset": {
        "baseline_marker": "initialized",
        "phase1_start_marker": "hotset_load_start",
        "phase1_end_marker": "hotset_update_start",
        "phase2_start_marker": "hotset_update_start",
        "phase2_end_marker": "hotset_done",
        "phase1_label": "Load",
        "phase2_label": "Concurrent hot-set RW",
    },
}

# Fixed color mapping for consistency across figures
COLOR_MAP = {
    "AVB": "#1f77b4",         # blue
    "AVB-dual": "#ff7f0e",    # orange
    "GTX": "#d62728",         # red
    "SLT": "#2ca02c",         # green
    "Teseo": "#8c564b",       # brown
    "RapidStore": "#9467bd",  # purple
    "LiveGraph": "#17becf",   # cyan
}

DEFAULT_LINEWIDTH = 1.5


def parse_runs(run_args):
    runs = []
    for item in run_args:
        if "=" not in item:
            raise SystemExit(f'Bad --run "{item}". Expected NAME=PREFIX')
        name, prefix = item.split("=", 1)
        runs.append((name.strip(), prefix.strip()))
    return runs


def load_run(prefix):
    mem_path = Path(prefix + ".mem.csv")
    ev_path = Path(prefix + ".events.csv")
    mem = pd.read_csv(mem_path)
    ev = pd.read_csv(ev_path)

    markers = {}
    for _, row in ev.iterrows():
        markers.setdefault(str(row["marker"]), float(row["time_sec"]))

    return mem, markers


def get_marker(markers, name):
    if name not in markers:
        raise KeyError(name)
    return float(markers[name])


def format_duration(seconds):
    seconds = float(seconds)

    if seconds < 60:
        return f"{seconds:.1f}s"

    minutes = int(seconds // 60)
    remain = seconds - minutes * 60

    if minutes < 60:
        if remain < 0.5:
            return f"{minutes}m"
        return f"{minutes}m {int(round(remain))}s"

    hours = int(minutes // 60)
    minutes = minutes % 60
    return f"{hours}h {minutes}m"


def build_combined_curve(mem, markers, split=0.10, samples_insert=80, samples_conc=320, mode="concurrent"):
    config = MODE_CONFIG[mode]
    t_base = get_marker(markers, config["baseline_marker"])
    t_i0 = get_marker(markers, config["phase1_start_marker"])
    t_i1 = get_marker(markers, config["phase1_end_marker"])
    t_c0 = get_marker(markers, config["phase2_start_marker"])
    t_c1 = get_marker(markers, config["phase2_end_marker"])

    base_rows = mem[mem["time_sec"] >= t_base]
    baseline_rss_mb = (
        float(base_rows["rss_mb"].iloc[0])
        if len(base_rows)
        else float(mem["rss_mb"].iloc[0])
    )

    insert_df = mem[(mem["time_sec"] >= t_i0) & (mem["time_sec"] <= t_i1)].copy()
    conc_df = mem[(mem["time_sec"] >= t_c0) & (mem["time_sec"] <= t_c1)].copy()

    if len(insert_df) < 2 or len(conc_df) < 2:
        return None

    insert_x = (insert_df["time_sec"] - t_i0) / max(t_i1 - t_i0, 1e-12)
    insert_y = insert_df["rss_mb"] - baseline_rss_mb

    conc_x = (conc_df["time_sec"] - t_c0) / max(t_c1 - t_c0, 1e-12)
    conc_y = conc_df["rss_mb"] - baseline_rss_mb

    gx_i = np.linspace(0.0, 1.0, samples_insert)
    gy_i = np.interp(gx_i, insert_x, insert_y)
    plot_x_i = split * gx_i

    gx_c = np.linspace(0.0, 1.0, samples_conc)
    gy_c = np.interp(gx_c, conc_x, conc_y)
    plot_x_c = split + (1.0 - split) * gx_c

    plot_x = np.concatenate([plot_x_i, plot_x_c])
    plot_y_gb = np.concatenate([gy_i, gy_c]) / 1024.0

    rss_insert_end_mb = float(insert_df["rss_mb"].iloc[-1])
    peak_conc_rss_mb = float(conc_df["rss_mb"].max())
    total_duration_sec = (t_i1 - t_i0) + (t_c1 - t_c0)

    summary = {
        "mode": mode,
        "phase1_label": config["phase1_label"],
        "phase2_label": config["phase2_label"],
        "baseline_rss_gb": baseline_rss_mb / 1024.0,
        "rss_insert_end_gb": rss_insert_end_mb / 1024.0,
        "peak_conc_rss_gb": peak_conc_rss_mb / 1024.0,
        "delta_insert_end_from_baseline_gb": (rss_insert_end_mb - baseline_rss_mb) / 1024.0,
        "delta_peak_conc_from_baseline_gb": (peak_conc_rss_mb - baseline_rss_mb) / 1024.0,
        "insert_duration_sec": t_i1 - t_i0,
        "conc_duration_sec": t_c1 - t_c0,
        "total_duration_sec": total_duration_sec,
    }

    return plot_x, plot_y_gb, summary


def main():
    ap = argparse.ArgumentParser(
        description="Merge two benchmark phases into one normalized memory-progress plot."
    )
    ap.add_argument("--run", action="append", required=True, help="NAME=PREFIX")
    ap.add_argument("--out", required=True, help="Output PNG path")
    ap.add_argument("--summary-out", default="", help="Optional CSV summary path")
    ap.add_argument(
        "--mode",
        choices=sorted(MODE_CONFIG),
        default="concurrent",
        help="Phase markers and labels to use, default concurrent",
    )
    ap.add_argument("--split", type=float, default=0.10, help="Fraction reserved for insert phase, default 0.10")
    args = ap.parse_args()

    runs = parse_runs(args.run)

    fig, ax = plt.subplots(figsize=(8, 3))
    summaries = []

    # Background phase regions
    ax.axvspan(
        0.0,
        args.split,
        facecolor="#f2f2f2",
        alpha=0.75,
        zorder=0,
        linewidth=0,
    )
    ax.axvspan(
        args.split,
        1.0,
        facecolor="#fafafa",
        alpha=0.75,
        zorder=0,
        linewidth=0,
    )

    # Subtle phase boundary
    ax.axvline(
        args.split,
        linestyle="-",
        linewidth=0.8,
        color="0.55",
        alpha=0.75,
        zorder=1,
    )

    for name, prefix in runs:
        try:
            mem, markers = load_run(prefix)
            result = build_combined_curve(
                mem, markers, mode=args.mode, split=args.split
            )

            if result is None:
                print(f"[WARN] {name}: too few samples; skipping")
                continue

            x, y_gb, summary = result

        except Exception as e:
            print(f"[WARN] {name}: {e}; skipping")
            continue

        color = COLOR_MAP.get(name, None)
        legend_label = f"{name} ({format_duration(summary['total_duration_sec'])})"

        ax.plot(
            x,
            y_gb,
            label=legend_label,
            linewidth=DEFAULT_LINEWIDTH,
            color=color,
            zorder=3,
        )

        summary["system"] = name
        summary["legend_label"] = legend_label
        summaries.append(summary)

    # Phase labels placed at the top of the plotting area.
    # x uses data coordinates, y uses axes coordinates.
    ax.text(
        args.split / 2.0,
        0.965,
        MODE_CONFIG[args.mode]["phase1_label"],
        transform=ax.get_xaxis_transform(),
        ha="center",
        va="top",
        fontsize=14,
    )
    ax.text(
        args.split + (1.0 - args.split) / 2.0,
        0.965,
        MODE_CONFIG[args.mode]["phase2_label"],
        transform=ax.get_xaxis_transform(),
        ha="center",
        va="top",
        fontsize=14,
    )

    ax.set_xlim(0.0, 1.0)
    ax.set_xlabel("Progress", fontsize=14)
    ax.set_ylabel("Consumption [GB]", fontsize=14)
    
    # Font size for axis tick labels.
    ax.tick_params(axis='both', which='major', labelsize=14) 

    ax.grid(True, alpha=0.3, zorder=2)

    ax.legend(
        loc="lower center",
        bbox_to_anchor=(0.5, 1.05),
        ncol=max(1, min(len(ax.lines), 5)),
        frameon=True,
        fancybox=False,
        borderpad=0.3,
        handlelength=1.4,
        handletextpad=0.4,
        columnspacing=0.8,
        fontsize=12,
    )

    fig.tight_layout()

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_path, dpi=200, bbox_inches="tight")
    plt.close(fig)

    if args.summary_out:
        summary_path = Path(args.summary_out)
        summary_path.parent.mkdir(parents=True, exist_ok=True)
        pd.DataFrame(summaries).to_csv(summary_path, index=False)

    print(f"Wrote {out_path}")

    if args.summary_out:
        print(f"Wrote {args.summary_out}")


if __name__ == "__main__":
    main()
