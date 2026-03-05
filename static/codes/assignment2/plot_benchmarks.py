#!/usr/bin/env python3
import argparse
import csv
import os
from typing import List

import matplotlib.pyplot as plt


def read_latency_file(path: str) -> List[float]:
    vals: List[float] = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            vals.append(float(line))
    return vals


def read_stress_csv(path: str):
    rows = []
    with open(path, "r", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for r in reader:
            rows.append(r)
    return rows


def plot_latency_distribution(out_path: str, thread_vals: List[float], fork_vals: List[float], select_vals: List[float]) -> None:
    plt.figure(figsize=(10, 6))
    data = [thread_vals, fork_vals, select_vals]
    labels = ["thread", "fork", "select"]
    plt.boxplot(data, labels=labels, showfliers=False)
    plt.ylabel("Message delivery latency (us)")
    plt.title("Latency distribution comparison")
    plt.grid(axis="y", linestyle="--", alpha=0.4)
    plt.tight_layout()
    plt.savefig(out_path, dpi=140)
    plt.close()


def plot_cpu_memory_vs_clients(out_path: str, thread_csv: str, fork_csv: str, select_csv: str) -> None:
    variants = [
        ("thread", thread_csv, "#1f77b4"),
        ("fork", fork_csv, "#d62728"),
        ("select", select_csv, "#2ca02c"),
    ]

    plt.figure(figsize=(12, 8))

    plt.subplot(2, 1, 1)
    for label, path, color in variants:
        rows = read_stress_csv(path)
        xs = [int(r["clients"]) for r in rows]
        ys = [float(r.get("avg_cpu_percent", 0.0)) for r in rows]
        plt.plot(xs, ys, marker="o", color=color, label=label)
    plt.ylabel("Avg CPU (%)")
    plt.title("CPU usage vs number of clients")
    plt.grid(True, linestyle="--", alpha=0.4)
    plt.legend()

    plt.subplot(2, 1, 2)
    for label, path, color in variants:
        rows = read_stress_csv(path)
        xs = [int(r["clients"]) for r in rows]
        ys = [float(r.get("peak_pss_kb", 0.0)) for r in rows]
        plt.plot(xs, ys, marker="o", color=color, label=label)
    plt.xlabel("Number of clients")
    plt.ylabel("Peak PSS (KB)")
    plt.title("Memory usage vs number of clients")
    plt.grid(True, linestyle="--", alpha=0.4)
    plt.legend()

    plt.tight_layout()
    plt.savefig(out_path, dpi=140)
    plt.close()


def main() -> None:
    ap = argparse.ArgumentParser(description="Plot benchmark comparison charts")
    ap.add_argument("--lat-thread", required=True, help="latencies_us.txt for thread run")
    ap.add_argument("--lat-fork", required=True, help="latencies_us.txt for fork run")
    ap.add_argument("--lat-select", required=True, help="latencies_us.txt for select run")
    ap.add_argument("--stress-thread", required=True, help="stress_results.csv for thread run")
    ap.add_argument("--stress-fork", required=True, help="stress_results.csv for fork run")
    ap.add_argument("--stress-select", required=True, help="stress_results.csv for select run")
    ap.add_argument("--out-dir", default="reports/plots", help="output directory")
    args = ap.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)

    thread_vals = read_latency_file(args.lat_thread)
    fork_vals = read_latency_file(args.lat_fork)
    select_vals = read_latency_file(args.lat_select)

    plot_latency_distribution(
        os.path.join(args.out_dir, "latency_distribution_comparison.png"),
        thread_vals,
        fork_vals,
        select_vals,
    )
    plot_cpu_memory_vs_clients(
        os.path.join(args.out_dir, "cpu_memory_vs_clients.png"),
        args.stress_thread,
        args.stress_fork,
        args.stress_select,
    )


if __name__ == "__main__":
    main()
