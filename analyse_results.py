#!/usr/bin/env python3
"""
analyse_results.py
==================
Generates all Section V metrics for the AFS (Adaptive Fast-Start) project.

Run after all ns-3 simulations complete (use run_all_sims.sh).

Usage:
    python3 analyse_results.py \\
        --results-dir ./results \\
        --output-dir  ./plots

File naming convention expected:
    fct_cubic_seed<N>.csv
    fct_afs_seed<N>_k<K*100>.csv      e.g. fct_afs_seed1_k200.csv
    retransmit_cubic_seed<N>.csv
    retransmit_afs_seed<N>_k<K*100>.csv
    queue_cubic_seed<N>.csv
    queue_afs_seed<N>_k<K*100>.csv
    summary_cubic_seed<N>.txt
    summary_afs_seed<N>_k<K*100>.txt

Outputs:
    fct_cdf_by_size.png       FCT CDF split by flow size bucket
    fct_slowdown.png          FCT slowdown ratio CDF
    retransmit_bar.png        Retransmission rate per seed
    elephant_throughput.png   Long-flow throughput comparison
    queue_cdf.png             Bottleneck queue occupancy CDF
    k_sweep.png               Retransmit rate vs k threshold
    summary_table.txt         All scalar metrics in one place

Requirements:
    pip install matplotlib pandas numpy scipy
"""

import csv
import glob
import os
import sys
import statistics
import argparse
from pathlib import Path

import numpy as np

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    HAS_MPL = True
except ImportError:
    HAS_MPL = False
    print("WARNING: matplotlib not found — skipping plots, printing tables only.")

# ── Constants matching simulation parameters ───────────────────────────────────

BOTTLENECK_BPS = 10e6   # 10 Mbps
MSS            = 1460   # bytes
RTT_S          = 0.022  # 22 ms base RTT (10 ms + 10 ms bottleneck + 2×1 ms access)
BN_BUFFER_B    = 25000  # 25 KB bottleneck buffer

# Flow size buckets for stratified analysis
BUCKETS = [
    ("XS (<20 KB)",     0,       20_000),
    ("S  (20–50 KB)",   20_000,  50_000),
    ("M  (50–100 KB)",  50_000, 100_000),
    ("L  (>100 KB)",   100_000, 999_999_999),
]

# k values evaluated
K_VALUES   = [1.5, 2.0, 2.5, 3.0]
K_TAGS     = [150, 200, 250, 300]   # k*100 as used in filenames
N_ELEPHANTS = 5

# Plot colours
COL_CUBIC = "#1F4E79"
COL_AFS   = "#C00000"
COL_SWEEP = ["#2E75B6", "#C00000", "#375623", "#7030A0"]

# ── CSV loaders ────────────────────────────────────────────────────────────────

def load_fct(path):
    """Load FCT CSV → list of dicts {flow_id, size_bytes, fct}."""
    rows = []
    if not os.path.exists(path):
        return rows
    with open(path) as f:
        for row in csv.DictReader(f):
            try:
                rows.append({
                    "flow_id":    int(row["flow_id"]),
                    "size_bytes": int(row["size_bytes"]),
                    "fct":        float(row["fct"]),
                })
            except (ValueError, KeyError):
                pass
    return rows


def load_retx(path):
    """Load retransmit CSV → list of dicts {flow_id, tx, retx, throughput}."""
    rows = []
    if not os.path.exists(path):
        return rows
    with open(path) as f:
        for row in csv.DictReader(f):
            try:
                rows.append({
                    "flow_id":    int(row["flow_id"]),
                    "tx":         int(row["tx_packets"]),
                    "retx":       int(row.get("retx_packets", 0)),
                    "throughput": float(row["throughput_mbps"]),
                })
            except (ValueError, KeyError):
                pass
    return rows


def load_queue(path):
    """Load queue CSV → list of byte counts."""
    vals = []
    if not os.path.exists(path):
        return vals
    with open(path) as f:
        for row in csv.DictReader(f):
            try:
                vals.append(int(row["bytes"]))
            except (ValueError, KeyError):
                pass
    return vals


def parse_retx_rate_from_summary(path):
    """Extract retransmission rate (%) from a summary .txt file."""
    if not os.path.exists(path):
        return None
    with open(path) as f:
        for line in f:
            if "Retransmission rate:" in line:
                try:
                    return float(line.split(":")[1].strip().replace("%", ""))
                except ValueError:
                    pass
    return None

# ── Data loader ────────────────────────────────────────────────────────────────

def load_all(results_dir):
    d = results_dir
    data = {}

    # Cubic (all seeds)
    data["cubic_fct"]  = []
    data["cubic_retx"] = []
    data["cubic_queue"]= []
    for seed in range(1, 6):
        data["cubic_fct"]  += load_fct  (f"{d}/fct_cubic_seed{seed}.csv")
        data["cubic_retx"] += load_retx (f"{d}/retransmit_cubic_seed{seed}.csv")
        data["cubic_queue"]+= load_queue(f"{d}/queue_cubic_seed{seed}.csv")

    # AFS k=2.0, all seeds (primary comparison)
    data["afs_fct"]   = []
    data["afs_retx"]  = []
    data["afs_queue"] = []
    for seed in range(1, 6):
        data["afs_fct"]  += load_fct  (f"{d}/fct_afs_seed{seed}_k200.csv")
        data["afs_retx"] += load_retx (f"{d}/retransmit_afs_seed{seed}_k200.csv")
        data["afs_queue"]+= load_queue(f"{d}/queue_afs_seed{seed}_k200.csv")

    # k sweep (seed 1 only for sensitivity analysis)
    data["sweep_fct"]  = {}
    data["sweep_retx"] = {}
    for k, ktag in zip(K_VALUES, K_TAGS):
        data["sweep_fct"][k]  = load_fct (f"{d}/fct_afs_seed1_k{ktag}.csv")
        data["sweep_retx"][k] = load_retx(f"{d}/retransmit_afs_seed1_k{ktag}.csv")

    return data

# ── Statistics helpers ─────────────────────────────────────────────────────────

def fct_stats(rows):
    fcts = sorted(r["fct"] for r in rows)
    n = len(fcts)
    if n == 0:
        return {"n": 0, "mean": 0, "median": 0, "p95": 0, "p99": 0, "max": 0}
    return {
        "n":      n,
        "mean":   statistics.mean(fcts),
        "median": statistics.median(fcts),
        "p95":    fcts[min(int(0.95 * n), n - 1)],
        "p99":    fcts[min(int(0.99 * n), n - 1)],
        "max":    fcts[-1],
    }


def ideal_fct(size_bytes, iw_mss=10):
    """Minimum possible FCT given IW and exponential slow-start."""
    iw_b = iw_mss * MSS
    delivered = iw_b
    rtts = 1
    cwnd = iw_b
    while delivered < size_bytes:
        cwnd = min(cwnd * 2, size_bytes)
        delivered += cwnd
        rtts += 1
    return rtts * RTT_S


def stratified(rows, iw_mss=10):
    result = {}
    for label, lo, hi in BUCKETS:
        subset = [r for r in rows if lo <= r["size_bytes"] < hi]
        if not subset:
            result[label] = None
            continue
        fcts = sorted(r["fct"] for r in subset)
        slowdowns = [r["fct"] / ideal_fct(r["size_bytes"], iw_mss) for r in subset]
        n = len(fcts)
        result[label] = {
            "n":             n,
            "mean_fct":      statistics.mean(fcts),
            "p95_fct":       fcts[min(int(0.95 * n), n - 1)],
            "mean_slowdown": statistics.mean(slowdowns),
        }
    return result


def elephant_tput(retx_rows):
    """Extract throughput for elephant flows (flow_id 1–5)."""
    return [r["throughput"] for r in retx_rows if r["flow_id"] <= N_ELEPHANTS]


def retx_rate(retx_rows, all_flows=True):
    """Overall retransmission rate (%)."""
    tx   = sum(r["tx"]   for r in retx_rows)
    retx = sum(r["retx"] for r in retx_rows)
    return (retx / tx * 100.0) if tx > 0 else 0.0


def jain(vals):
    n = len(vals)
    if n == 0:
        return 0.0
    return (sum(vals) ** 2) / (n * sum(x ** 2 for x in vals))


def cdf(vals):
    s = sorted(vals)
    n = len(s)
    return s, [i / n for i in range(n)]

# ── Plot functions ─────────────────────────────────────────────────────────────

def plot_fct_cdf(data, out_dir):
    if not HAS_MPL:
        return
    fig, axes = plt.subplots(1, 2, figsize=(12, 4.5))

    for ax, (label, lo, hi) in zip(axes, BUCKETS[1:3]):
        cx, cy = cdf([r["fct"] for r in data["cubic_fct"] if lo <= r["size_bytes"] < hi])
        ax_x, ay = cdf([r["fct"] for r in data["afs_fct"]   if lo <= r["size_bytes"] < hi])
        if cx:
            ax.plot(cx, cy, color=COL_CUBIC, lw=2, label="TCP Cubic")
        if ax_x:
            ax.plot(ax_x, ay, color=COL_AFS, lw=2, ls="--", label="AFS (k=2.0)")
        ax.set_xlabel("Flow Completion Time (s)", fontsize=10)
        ax.set_ylabel("CDF", fontsize=10)
        ax.set_title(f"FCT CDF — {label}", fontsize=11, fontweight="bold")
        ax.legend(fontsize=9)
        ax.grid(True, alpha=0.3)
        ax.set_xlim(left=0)

    plt.tight_layout()
    out = f"{out_dir}/fct_cdf_by_size.png"
    plt.savefig(out, dpi=150, bbox_inches="tight")
    plt.close()
    print(f"  Saved: {out}")


def plot_slowdown(data, out_dir):
    if not HAS_MPL:
        return
    fig, ax = plt.subplots(figsize=(8, 4.5))
    colours = [COL_CUBIC, "#2E75B6", "#375623", COL_AFS]

    for (label, lo, hi), col in zip(BUCKETS, colours):
        c_sub = [r for r in data["cubic_fct"] if lo <= r["size_bytes"] < hi]
        a_sub = [r for r in data["afs_fct"]   if lo <= r["size_bytes"] < hi]
        if not c_sub or not a_sub:
            continue
        csd = sorted(r["fct"] / ideal_fct(r["size_bytes"], 10) for r in c_sub)
        asd = sorted(r["fct"] / ideal_fct(r["size_bytes"], 20) for r in a_sub)
        ax.plot(csd, [i / len(csd) for i in range(len(csd))],
                color=col, lw=2, label=f"Cubic {label}")
        ax.plot(asd, [i / len(asd) for i in range(len(asd))],
                color=col, lw=2, ls="--", alpha=0.6, label=f"AFS {label}")

    ax.set_xlabel("FCT Slowdown (FCT / Ideal FCT)", fontsize=10)
    ax.set_ylabel("CDF", fontsize=10)
    ax.set_title("FCT Slowdown Ratio by Flow Size Bucket", fontsize=11, fontweight="bold")
    ax.legend(fontsize=7, ncol=2)
    ax.grid(True, alpha=0.3)
    ax.set_xlim(0, 10)
    plt.tight_layout()
    out = f"{out_dir}/fct_slowdown.png"
    plt.savefig(out, dpi=150, bbox_inches="tight")
    plt.close()
    print(f"  Saved: {out}")


def plot_retransmit_bar(results_dir, out_dir):
    if not HAS_MPL:
        return
    seeds = list(range(1, 6))
    c_rates = [parse_retx_rate_from_summary(f"{results_dir}/summary_cubic_seed{s}.txt") or 0
               for s in seeds]
    a_rates = [parse_retx_rate_from_summary(f"{results_dir}/summary_afs_seed{s}_k200.txt") or 0
               for s in seeds]

    x = np.arange(len(seeds))
    w = 0.35
    fig, ax = plt.subplots(figsize=(8, 4))
    ax.bar(x - w/2, c_rates, w, label="TCP Cubic",  color=COL_CUBIC, alpha=0.9)
    ax.bar(x + w/2, a_rates, w, label="AFS (k=2.0)", color=COL_AFS,  alpha=0.9)

    if any(c_rates):
        ax.axhline(statistics.mean([r for r in c_rates if r]),
                   color=COL_CUBIC, ls="--", lw=1, alpha=0.6,
                   label=f"Cubic mean ({statistics.mean(c_rates):.1f}%)")
    if any(a_rates):
        ax.axhline(statistics.mean([r for r in a_rates if r]),
                   color=COL_AFS, ls="--", lw=1, alpha=0.6,
                   label=f"AFS mean ({statistics.mean(a_rates):.1f}%)")

    ax.set_xlabel("Random Seed", fontsize=10)
    ax.set_ylabel("Retransmission Rate (%)", fontsize=10)
    ax.set_title("Retransmission Rate: Cubic vs AFS (5 Seeds)", fontsize=11, fontweight="bold")
    ax.set_xticks(x)
    ax.set_xticklabels([f"Seed {s}" for s in seeds])
    ax.legend(fontsize=8)
    ax.grid(True, alpha=0.3, axis="y")
    ax.set_ylim(0, 25)
    plt.tight_layout()
    out = f"{out_dir}/retransmit_bar.png"
    plt.savefig(out, dpi=150, bbox_inches="tight")
    plt.close()
    print(f"  Saved: {out}")


def plot_k_sweep(results_dir, out_dir):
    if not HAS_MPL:
        return
    rates = []
    for k, ktag in zip(K_VALUES, K_TAGS):
        r = parse_retx_rate_from_summary(f"{results_dir}/summary_afs_seed1_k{ktag}.txt")
        rates.append(r or 0)

    cubic_rate = parse_retx_rate_from_summary(f"{results_dir}/summary_cubic_seed1.txt") or 0

    fig, ax = plt.subplots(figsize=(7, 4))
    ax.bar([str(k) for k in K_VALUES], rates, color=COL_AFS, alpha=0.85, width=0.4, label="AFS")
    ax.axhline(cubic_rate, color=COL_CUBIC, ls="--", lw=2,
               label=f"Cubic baseline ({cubic_rate:.1f}%)")
    ax.set_xlabel("Exit multiplier k", fontsize=10)
    ax.set_ylabel("Retransmission Rate (%)", fontsize=10)
    ax.set_title("AFS Sensitivity to k (Seed 1)", fontsize=11, fontweight="bold")
    ax.legend(fontsize=9)
    ax.grid(True, alpha=0.3, axis="y")
    ax.set_ylim(0, 25)
    plt.tight_layout()
    out = f"{out_dir}/k_sweep.png"
    plt.savefig(out, dpi=150, bbox_inches="tight")
    plt.close()
    print(f"  Saved: {out}")


def plot_queue_cdf(data, out_dir):
    if not HAS_MPL:
        return
    fig, ax = plt.subplots(figsize=(7, 4))
    cq = sorted(data["cubic_queue"])
    aq = sorted(data["afs_queue"])

    if cq:
        ax.plot([x / BN_BUFFER_B * 100 for x in cq],
                [i / len(cq) for i in range(len(cq))],
                color=COL_CUBIC, lw=2, label="TCP Cubic")
    if aq:
        ax.plot([x / BN_BUFFER_B * 100 for x in aq],
                [i / len(aq) for i in range(len(aq))],
                color=COL_AFS, lw=2, ls="--", label="AFS (k=2.0)")

    ax.set_xlabel("Queue Occupancy (% of 25 KB buffer)", fontsize=10)
    ax.set_ylabel("CDF", fontsize=10)
    ax.set_title("Bottleneck Queue Occupancy CDF", fontsize=11, fontweight="bold")
    ax.legend(fontsize=9)
    ax.grid(True, alpha=0.3)
    ax.set_xlim(0, 105)
    plt.tight_layout()
    out = f"{out_dir}/queue_cdf.png"
    plt.savefig(out, dpi=150, bbox_inches="tight")
    plt.close()
    print(f"  Saved: {out}")


def plot_elephant_throughput(data, out_dir):
    if not HAS_MPL:
        return
    ct = elephant_tput(data["cubic_retx"])
    at = elephant_tput(data["afs_retx"])
    if not ct or not at:
        return

    fig, ax = plt.subplots(figsize=(6, 4))
    means = [statistics.mean(ct), statistics.mean(at)]
    stds  = [statistics.stdev(ct) if len(ct) > 1 else 0,
             statistics.stdev(at) if len(at) > 1 else 0]
    ax.bar(["TCP Cubic", "AFS (k=2.0)"], means, color=[COL_CUBIC, COL_AFS], alpha=0.9, width=0.4)
    ax.errorbar(["TCP Cubic", "AFS (k=2.0)"], means, yerr=stds,
                fmt="none", color="black", capsize=5)
    ax.set_ylabel("Mean Elephant Throughput (Mbps)", fontsize=10)
    ax.set_title("Long-Flow Throughput: Cubic vs AFS", fontsize=11, fontweight="bold")
    ax.grid(True, alpha=0.3, axis="y")
    plt.tight_layout()
    out = f"{out_dir}/elephant_throughput.png"
    plt.savefig(out, dpi=150, bbox_inches="tight")
    plt.close()
    print(f"  Saved: {out}")

# ── Summary table ──────────────────────────────────────────────────────────────

def print_summary(data, results_dir, out_dir):
    lines = []
    H = "=" * 72

    lines.append(H)
    lines.append("AFS PROJECT — SECTION V SUMMARY TABLE")
    lines.append("Authors: Anshuman Dave (231EE107) | Purva Siddapurmath (231EE242)")
    lines.append(H)

    # 1. Overall FCT
    cs = fct_stats(data["cubic_fct"])
    as_ = fct_stats(data["afs_fct"])
    lines.append("\n1. OVERALL SHORT-FLOW FCT (all mice, 5 seeds)")
    lines.append(f"  {'Metric':<18} {'Cubic':>12} {'AFS k=2.0':>14} {'Change':>10}")
    lines.append("  " + "-" * 56)
    for key, label in [("mean","Mean"),("median","Median"),("p95","P95"),("p99","P99")]:
        cv, av = cs[key], as_[key]
        chg = (av - cv) / cv * 100 if cv else 0
        lines.append(f"  {label+' FCT (s)':<18} {cv:>12.4f} {av:>14.4f} {chg:>+9.1f}%")
    lines.append(f"  {'N flows':<18} {cs['n']:>12} {as_['n']:>14}")

    # 2. Stratified FCT
    lines.append("\n2. STRATIFIED FCT (mean, by flow size)")
    cstr = stratified(data["cubic_fct"], 10)
    astr = stratified(data["afs_fct"],   20)
    hdr = f"  {'Bucket':<18} {'N':>5} {'Cubic FCT':>12} {'AFS FCT':>12} {'Change':>9} {'C Slowdown':>11} {'AFS Slowdown':>13}"
    lines.append(hdr)
    lines.append("  " + "-" * 84)
    for label, lo, hi in BUCKETS:
        cs2 = cstr.get(label)
        as2 = astr.get(label)
        if cs2 and as2:
            chg = (as2["mean_fct"] - cs2["mean_fct"]) / cs2["mean_fct"] * 100
            lines.append(
                f"  {label:<18} {cs2['n']:>5} {cs2['mean_fct']:>12.4f} "
                f"{as2['mean_fct']:>12.4f} {chg:>+8.1f}% "
                f"{cs2['mean_slowdown']:>11.2f}x {as2['mean_slowdown']:>12.2f}x"
            )

    # 3. Retransmission
    c_rates = [parse_retx_rate_from_summary(f"{results_dir}/summary_cubic_seed{s}.txt") or 0
               for s in range(1, 6)]
    a_rates = [parse_retx_rate_from_summary(f"{results_dir}/summary_afs_seed{s}_k200.txt") or 0
               for s in range(1, 6)]
    c_mean = statistics.mean(c_rates) if c_rates else 0
    a_mean = statistics.mean(a_rates) if a_rates else 0
    lines.append("\n3. RETRANSMISSION RATES (5 seeds, k=2.0)")
    lines.append(f"  Cubic  mean={c_mean:.2f}%  per-seed: {[f'{r:.1f}' for r in c_rates]}")
    lines.append(f"  AFS    mean={a_mean:.2f}%  per-seed: {[f'{r:.1f}' for r in a_rates]}")
    lines.append(f"  Ratio AFS/Cubic: {a_mean/c_mean:.2f}×  (target ≤ 1.5×)")

    # 4. k sweep
    lines.append("\n4. k SENSITIVITY (seed 1, retransmit rate %)")
    for k, ktag in zip(K_VALUES, K_TAGS):
        r = parse_retx_rate_from_summary(f"{results_dir}/summary_afs_seed1_k{ktag}.txt")
        lines.append(f"  k={k:.1f}: {r:.2f}%" if r is not None else f"  k={k:.1f}: N/A")

    # 5. Elephant throughput
    ct = elephant_tput(data["cubic_retx"])
    at = elephant_tput(data["afs_retx"])
    lines.append("\n5. LONG-FLOW (ELEPHANT) THROUGHPUT")
    if ct:
        lines.append(f"  Cubic  mean={statistics.mean(ct):.3f} Mbps  Jain={jain(ct):.4f}")
    if at:
        lines.append(f"  AFS    mean={statistics.mean(at):.3f} Mbps  Jain={jain(at):.4f}")
    if ct and at:
        delta = (statistics.mean(at) - statistics.mean(ct)) / statistics.mean(ct) * 100
        lines.append(f"  Change: {delta:+.1f}%  (target < 2% regression)")

    # 6. Queue
    cq = data["cubic_queue"]
    aq = data["afs_queue"]
    lines.append("\n6. BOTTLENECK QUEUE OCCUPANCY (buffer = 25 000 B)")
    if cq:
        lines.append(f"  Cubic  mean={statistics.mean(cq):.0f} B  max={max(cq)} B")
    if aq:
        lines.append(f"  AFS    mean={statistics.mean(aq):.0f} B  max={max(aq)} B")
    if cq and aq:
        qdelta = (statistics.mean(aq) - statistics.mean(cq)) / statistics.mean(cq) * 100
        lines.append(f"  Mean queue change: {qdelta:+.1f}%")

    lines.append("\n" + H)

    text = "\n".join(lines)
    print(text)

    out = f"{out_dir}/summary_table.txt"
    with open(out, "w") as f:
        f.write(text + "\n")
    print(f"\n  Summary written to: {out}")

# ── Main ───────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Analyse AFS ns-3 simulation results"
    )
    parser.add_argument("--results-dir", default="./results",
                        help="Directory containing simulation CSV/txt files")
    parser.add_argument("--output-dir",  default="./plots",
                        help="Directory for output plots and summary")
    args = parser.parse_args()

    os.makedirs(args.output_dir, exist_ok=True)

    print(f"Loading data from: {args.results_dir}")
    data = load_all(args.results_dir)

    n_cubic = len(data["cubic_fct"])
    n_afs   = len(data["afs_fct"])
    print(f"  Cubic mouse flows loaded:  {n_cubic}")
    print(f"  AFS   mouse flows loaded:  {n_afs}")

    if n_cubic == 0 and n_afs == 0:
        print("\nWARNING: No FCT data found. Run simulations first.")
        print(f"Expected files like: {args.results_dir}/fct_cubic_seed1.csv")
        return

    print_summary(data, args.results_dir, args.output_dir)

    if HAS_MPL:
        print("\nGenerating plots...")
        plot_fct_cdf(data, args.output_dir)
        plot_slowdown(data, args.output_dir)
        plot_retransmit_bar(args.results_dir, args.output_dir)
        plot_k_sweep(args.results_dir, args.output_dir)
        plot_queue_cdf(data, args.output_dir)
        plot_elephant_throughput(data, args.output_dir)
        print(f"\nAll plots saved to: {args.output_dir}")
    else:
        print("\nInstall matplotlib to generate plots: pip install matplotlib")

    print("\nDone.")


if __name__ == "__main__":
    main()