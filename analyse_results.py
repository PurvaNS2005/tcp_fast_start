#!/usr/bin/env python3
"""
analyse_results.py
==================
Generates all Section V metrics for the Fast-Start progress report.
Run this after your ns-3 simulations complete.

Usage:
    python3 analyse_results.py --results_dir ./results --output_dir ./plots

Requirements:
    pip install matplotlib pandas numpy scipy

What this produces:
    1. fct_stratified.png    — FCT CDF split by flow size category
    2. fct_slowdown.png      — FCT slowdown ratio (FCT / ideal_FCT)
    3. retransmit_bar.png    — Retransmission rate comparison across seeds
    4. throughput_bar.png    — Elephant throughput comparison
    5. queue_cdf.png         — Queue occupancy CDF
    6. threshold_sweep.png   — Retransmit rate vs EXIT_THRESHOLD
    7. summary_table.txt     — All numbers in one place for the report
"""

import csv, glob, os, sys, statistics, argparse
import numpy as np

# ── Optional matplotlib import ──────────────────────────────────────────────
try:
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    import matplotlib.ticker as ticker
    HAS_MPL = True
except ImportError:
    HAS_MPL = False
    print("WARNING: matplotlib not found. Skipping plots, printing tables only.")

# ── Config ───────────────────────────────────────────────────────────────────
BOTTLENECK_BPS  = 10e6          # 10 Mbps
MSS             = 1460          # bytes
RTT_S           = 0.022         # 22 ms base RTT

# Flow size buckets for stratified FCT
BUCKETS = [
    ("XS  (<20 KB)",   0,        20_000),
    ("S   (20–50 KB)", 20_000,   50_000),
    ("M   (50–100 KB)",50_000,  100_000),
    ("L   (>100 KB)", 100_000, 999_999_999),
]

NAVY  = "#1F4E79"
BLUE  = "#2E75B6"
RED   = "#C00000"
GREEN = "#375623"
GREY  = "#888888"

def ideal_fct(size_bytes, iw_mss=10):
    """
    Compute the ideal FCT (minimum possible) for a flow of given size.
    Ideal = RTTs needed × RTT_base, assuming perfect knowledge.
    """
    iw_bytes = iw_mss * MSS
    delivered = iw_bytes
    rtts = 1
    cwnd = iw_bytes
    while delivered < size_bytes:
        cwnd = min(cwnd * 2, size_bytes)  # slow start doubling
        delivered += cwnd
        rtts += 1
    return rtts * RTT_S

def load_fct_file(path):
    rows = []
    with open(path) as f:
        for row in csv.DictReader(f):
            rows.append({
                'flow_id':    int(row['flow_id']),
                'size_bytes': int(row['size_bytes']),
                'fct':        float(row['fct']),
            })
    return rows

def load_retransmit_file(path):
    rows = []
    with open(path) as f:
        for row in csv.DictReader(f):
            rows.append({
                'flow_id':   int(row['flow_id']),
                'tx':        int(row['tx_packets']),
                'retx':      int(row.get('retx_packets', 0)),
                'throughput':float(row['throughput_mbps']),
            })
    return rows

def load_queue_file(path):
    vals = []
    with open(path) as f:
        for row in csv.DictReader(f):
            vals.append(int(row['bytes']))
    return vals

# ── Load all data ─────────────────────────────────────────────────────────────
def load_all(results_dir):
    data = {}

    # Cubic FCT — all seeds
    data['cubic_fct'] = []
    for f in sorted(glob.glob(os.path.join(results_dir, 'fct_cubic_seed*.csv'))):
        data['cubic_fct'].extend(load_fct_file(f))

    # Fast-Start FCT th=1.10 — all seeds
    data['fs_fct_110'] = []
    for f in sorted(glob.glob(os.path.join(results_dir, 'fct_fast-start_seed*_th110.csv'))):
        data['fs_fct_110'].extend(load_fct_file(f))

    # Threshold sweep — seed 1
    for th in ['105', '110', '120']:
        key = f'fs_fct_{th}'
        path = os.path.join(results_dir, f'fct_fast-start_seed1_th{th}.csv')
        data[key] = load_fct_file(path) if os.path.exists(path) else []

    # Retransmit — cubic
    data['cubic_retx'] = []
    for f in sorted(glob.glob(os.path.join(results_dir, 'retransmit_cubic_seed*.csv'))):
        data['cubic_retx'].extend(load_retransmit_file(f))

    # Retransmit — fast-start th=1.10
    data['fs_retx_110'] = []
    for f in sorted(glob.glob(os.path.join(results_dir, 'retransmit_fast-start_seed*_th110.csv'))):
        data['fs_retx_110'].extend(load_retransmit_file(f))

    # Queue — cubic
    data['cubic_queue'] = []
    for f in sorted(glob.glob(os.path.join(results_dir, 'queue_cubic_seed*.csv'))):
        data['cubic_queue'].extend(load_queue_file(f))

    # Queue — fast-start th=1.10
    data['fs_queue_110'] = []
    for f in sorted(glob.glob(os.path.join(results_dir, 'queue_fast-start_seed*_th110.csv'))):
        data['fs_queue_110'].extend(load_queue_file(f))

    return data

# ── Statistics helpers ────────────────────────────────────────────────────────
def fct_stats(rows):
    fcts = sorted(r['fct'] for r in rows)
    n = len(fcts)
    return {
        'n':      n,
        'mean':   statistics.mean(fcts),
        'median': statistics.median(fcts),
        'p95':    fcts[int(0.95 * n)],
        'p99':    fcts[int(0.99 * n)],
        'max':    fcts[-1],
    }

def stratified_fct_stats(rows, iw_mss=10):
    result = {}
    for label, lo, hi in BUCKETS:
        subset = [r for r in rows if lo <= r['size_bytes'] < hi]
        if not subset:
            result[label] = None
            continue
        fcts = sorted(r['fct'] for r in subset)
        ideals = [ideal_fct(r['size_bytes'], iw_mss) for r in subset]
        slowdowns = [r['fct'] / ideal_fct(r['size_bytes'], iw_mss) for r in subset]
        n = len(fcts)
        result[label] = {
            'n':        n,
            'mean_fct': statistics.mean(fcts),
            'p95_fct':  fcts[int(0.95*n)],
            'mean_slowdown': statistics.mean(slowdowns),
            'p95_slowdown':  sorted(slowdowns)[int(0.95*n)],
        }
    return result

def elephant_tput(retx_rows):
    # Elephant flows are flow_id 1-5 (persistent flows in simulation)
    return [r['throughput'] for r in retx_rows if r['flow_id'] <= 5]

def jain(vals):
    n = len(vals)
    if n == 0: return 0
    return (sum(vals)**2) / (n * sum(x**2 for x in vals))

def retransmit_rate_from_summaries(results_dir, pattern):
    """Parse retransmit rate from summary .txt files."""
    rates = []
    for f in sorted(glob.glob(os.path.join(results_dir, pattern))):
        with open(f) as fp:
            for line in fp:
                if 'Retransmission rate:' in line:
                    rate = float(line.split(':')[1].strip().replace('%',''))
                    rates.append(rate)
    return rates

# ── Plot functions ─────────────────────────────────────────────────────────────
def plot_fct_cdf(data, output_dir):
    if not HAS_MPL: return
    fig, axes = plt.subplots(1, 2, figsize=(12, 4))

    for ax, (bucket_label, lo, hi) in zip(axes, BUCKETS[1:3]):  # S and M buckets
        cubic_fcts = sorted(r['fct'] for r in data['cubic_fct'] if lo <= r['size_bytes'] < hi)
        fs_fcts    = sorted(r['fct'] for r in data['fs_fct_110'] if lo <= r['size_bytes'] < hi)

        def cdf(vals):
            n = len(vals)
            return vals, [i/n for i in range(n)]

        if cubic_fcts:
            x, y = cdf(cubic_fcts)
            ax.plot(x, y, color=NAVY, linewidth=2, label='TCP Cubic')
        if fs_fcts:
            x, y = cdf(fs_fcts)
            ax.plot(x, y, color=RED,  linewidth=2, label='Fast-Start (th=1.10)', linestyle='--')

        ax.set_xlabel('Flow Completion Time (s)', fontsize=10)
        ax.set_ylabel('CDF', fontsize=10)
        ax.set_title(f'FCT CDF — {bucket_label}', fontsize=11, fontweight='bold')
        ax.legend(fontsize=9)
        ax.grid(True, alpha=0.3)
        ax.set_xlim(left=0)

    plt.tight_layout()
    out = os.path.join(output_dir, 'fct_stratified_cdf.png')
    plt.savefig(out, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"  Saved: {out}")

def plot_slowdown(data, output_dir):
    if not HAS_MPL: return
    fig, ax = plt.subplots(figsize=(8, 4))

    for (label, lo, hi), color, ls in zip(BUCKETS, [NAVY, BLUE, GREEN, RED], ['-','--','-.',':']):
        c_subset = [r for r in data['cubic_fct'] if lo <= r['size_bytes'] < hi]
        f_subset = [r for r in data['fs_fct_110'] if lo <= r['size_bytes'] < hi]
        if not c_subset or not f_subset: continue

        c_slowdowns = sorted(r['fct'] / ideal_fct(r['size_bytes'], 10) for r in c_subset)
        f_slowdowns = sorted(r['fct'] / ideal_fct(r['size_bytes'], 20) for r in f_subset)
        n = min(len(c_slowdowns), len(f_slowdowns))

        ax.plot(c_slowdowns, [i/len(c_slowdowns) for i in range(len(c_slowdowns))],
                color=color, linewidth=2, linestyle=ls, label=f'Cubic {label}')
        ax.plot(f_slowdowns, [i/len(f_slowdowns) for i in range(len(f_slowdowns))],
                color=color, linewidth=2, linestyle=ls, alpha=0.5, label=f'FastStart {label}')

    ax.set_xlabel('FCT Slowdown (FCT / Ideal FCT)', fontsize=10)
    ax.set_ylabel('CDF', fontsize=10)
    ax.set_title('FCT Slowdown Ratio by Flow Size', fontsize=11, fontweight='bold')
    ax.legend(fontsize=7, ncol=2)
    ax.grid(True, alpha=0.3)
    ax.set_xlim(0, 10)

    out = os.path.join(output_dir, 'fct_slowdown.png')
    plt.savefig(out, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"  Saved: {out}")

def plot_retransmit_bar(results_dir, output_dir):
    if not HAS_MPL: return

    cubic_rates = retransmit_rate_from_summaries(results_dir, 'summary_cubic_seed*.txt')
    fs_rates    = retransmit_rate_from_summaries(results_dir, 'summary_fast-start_seed*_th110.txt')

    seeds = list(range(1, len(cubic_rates)+1))
    x = np.arange(len(seeds))
    w = 0.35

    fig, ax = plt.subplots(figsize=(8, 4))
    ax.bar(x - w/2, cubic_rates, w, label='TCP Cubic',            color=NAVY,  alpha=0.9)
    ax.bar(x + w/2, fs_rates,    w, label='Fast-Start (th=1.10)', color=RED,   alpha=0.9)

    ax.axhline(statistics.mean(cubic_rates), color=NAVY, linestyle='--', linewidth=1, alpha=0.6, label=f'Cubic mean ({statistics.mean(cubic_rates):.1f}%)')
    ax.axhline(statistics.mean(fs_rates),    color=RED,  linestyle='--', linewidth=1, alpha=0.6, label=f'FS mean ({statistics.mean(fs_rates):.1f}%)')

    ax.set_xlabel('Random Seed', fontsize=10)
    ax.set_ylabel('Retransmission Rate (%)', fontsize=10)
    ax.set_title('Retransmission Rate: Cubic vs Fast-Start (Across Seeds)', fontsize=11, fontweight='bold')
    ax.set_xticks(x)
    ax.set_xticklabels([f'Seed {s}' for s in seeds])
    ax.legend(fontsize=9)
    ax.grid(True, alpha=0.3, axis='y')
    ax.set_ylim(0, 25)

    out = os.path.join(output_dir, 'retransmit_bar.png')
    plt.savefig(out, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"  Saved: {out}")

def plot_threshold_sweep(results_dir, output_dir):
    if not HAS_MPL: return

    thresholds = ['1.05', '1.10', '1.20']
    th_labels  = ['1.05', '1.10', '1.20']
    rates = []
    for th, thn in zip(['105','110','120'], thresholds):
        r = retransmit_rate_from_summaries(results_dir, f'summary_fast-start_seed1_th{th}.txt')
        rates.append(r[0] if r else 0)

    cubic_rate = retransmit_rate_from_summaries(results_dir, 'summary_cubic_seed1.txt')
    cubic_val  = cubic_rate[0] if cubic_rate else 8.4

    fig, ax = plt.subplots(figsize=(6, 4))
    ax.bar(th_labels, rates, color=BLUE, alpha=0.9, width=0.4, label='Fast-Start')
    ax.axhline(cubic_val, color=NAVY, linestyle='--', linewidth=2, label=f'Cubic baseline ({cubic_val:.1f}%)')

    ax.set_xlabel('EXIT_THRESHOLD', fontsize=10)
    ax.set_ylabel('Retransmission Rate (%)', fontsize=10)
    ax.set_title('Threshold Sensitivity (Seed 1)', fontsize=11, fontweight='bold')
    ax.legend(fontsize=9)
    ax.grid(True, alpha=0.3, axis='y')
    ax.set_ylim(0, 25)

    out = os.path.join(output_dir, 'threshold_sweep.png')
    plt.savefig(out, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"  Saved: {out}")

def plot_queue_cdf(data, output_dir):
    if not HAS_MPL: return

    cq = sorted(data['cubic_queue'])
    fq = sorted(data['fs_queue_110'])
    BUF = 25000  # bytes

    fig, ax = plt.subplots(figsize=(7, 4))
    ax.plot([x/BUF*100 for x in cq], [i/len(cq) for i in range(len(cq))],
            color=NAVY, linewidth=2, label='TCP Cubic')
    ax.plot([x/BUF*100 for x in fq], [i/len(fq) for i in range(len(fq))],
            color=RED, linewidth=2, linestyle='--', label='Fast-Start (th=1.10)')

    ax.set_xlabel('Queue Occupancy (% of buffer capacity)', fontsize=10)
    ax.set_ylabel('CDF', fontsize=10)
    ax.set_title('Bottleneck Queue Occupancy CDF', fontsize=11, fontweight='bold')
    ax.legend(fontsize=9)
    ax.grid(True, alpha=0.3)
    ax.set_xlim(0, 105)

    out = os.path.join(output_dir, 'queue_cdf.png')
    plt.savefig(out, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"  Saved: {out}")

def plot_elephant_throughput(data, output_dir):
    if not HAS_MPL: return

    c_tput = elephant_tput(data['cubic_retx'])
    f_tput = elephant_tput(data['fs_retx_110'])

    fig, ax = plt.subplots(figsize=(6, 4))
    ax.bar(['TCP Cubic', 'Fast-Start (th=1.10)'],
           [statistics.mean(c_tput), statistics.mean(f_tput)],
           color=[NAVY, RED], alpha=0.9, width=0.4)
    ax.errorbar(['TCP Cubic', 'Fast-Start (th=1.10)'],
                [statistics.mean(c_tput), statistics.mean(f_tput)],
                yerr=[statistics.stdev(c_tput), statistics.stdev(f_tput)],
                fmt='none', color='black', capsize=5)

    ax.set_ylabel('Mean Elephant Throughput (Mbps)', fontsize=10)
    ax.set_title('Long-Flow Throughput: Cubic vs Fast-Start', fontsize=11, fontweight='bold')
    ax.grid(True, alpha=0.3, axis='y')
    ax.set_ylim(0, 0.8)

    out = os.path.join(output_dir, 'elephant_throughput.png')
    plt.savefig(out, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"  Saved: {out}")

# ── Summary table ─────────────────────────────────────────────────────────────
def print_summary(data, results_dir):
    print("\n" + "="*70)
    print("SECTION V SUMMARY TABLE")
    print("="*70)

    # Overall FCT
    c_stats = fct_stats(data['cubic_fct'])
    f_stats = fct_stats(data['fs_fct_110'])
    print("\n1. OVERALL FCT (all short flows, 5 seeds)")
    print(f"   {'Metric':<20} {'Cubic':>12} {'FastStart th=1.10':>20} {'Change':>12}")
    print(f"   {'-'*64}")
    for key in ['n','mean','median','p95','p99']:
        c_val = c_stats[key]
        f_val = f_stats[key]
        if key == 'n':
            print(f"   {'N flows':<20} {c_val:>12} {f_val:>20}")
        else:
            chg = (f_val - c_val) / c_val * 100
            print(f"   {key.upper()+' FCT (s)':<20} {c_val:>12.4f} {f_val:>20.4f} {chg:>+11.1f}%")

    # Stratified FCT
    print("\n2. STRATIFIED FCT (mean, by flow size)")
    c_strat = stratified_fct_stats(data['cubic_fct'], 10)
    f_strat = stratified_fct_stats(data['fs_fct_110'], 20)
    print(f"   {'Bucket':<18} {'N(C)':>6} {'Mean FCT C':>12} {'Mean FCT FS':>14} {'Change':>10} {'Slowdown C':>12} {'Slowdown FS':>13}")
    print(f"   {'-'*89}")
    for label, lo, hi in BUCKETS:
        cs = c_strat.get(label)
        fs = f_strat.get(label)
        if cs and fs:
            chg = (fs['mean_fct'] - cs['mean_fct']) / cs['mean_fct'] * 100
            print(f"   {label:<18} {cs['n']:>6} {cs['mean_fct']:>12.4f} {fs['mean_fct']:>14.4f} {chg:>+10.1f}% {cs['mean_slowdown']:>12.2f}x {fs['mean_slowdown']:>13.2f}x")

    # Retransmit
    cubic_rates = retransmit_rate_from_summaries(results_dir, 'summary_cubic_seed*.txt')
    fs_rates    = retransmit_rate_from_summaries(results_dir, 'summary_fast-start_seed*_th110.txt')
    print("\n3. RETRANSMISSION RATES")
    print(f"   Cubic:      mean={statistics.mean(cubic_rates):.2f}%  sd={statistics.stdev(cubic_rates):.2f}%")
    print(f"   FastStart:  mean={statistics.mean(fs_rates):.2f}%  sd={statistics.stdev(fs_rates):.2f}%")
    print(f"   Ratio:      {statistics.mean(fs_rates)/statistics.mean(cubic_rates):.2f}x")

    # Threshold sweep
    print("\n4. THRESHOLD SENSITIVITY (seed 1, retransmit rate)")
    for th, thn in zip(['105','110','120'], ['1.05','1.10','1.20']):
        r = retransmit_rate_from_summaries(results_dir, f'summary_fast-start_seed1_th{th}.txt')
        if r: print(f"   th={thn}:  {r[0]:.2f}%")

    # Elephant throughput
    c_tput = elephant_tput(data['cubic_retx'])
    f_tput = elephant_tput(data['fs_retx_110'])
    print("\n5. LONG-FLOW (ELEPHANT) THROUGHPUT")
    print(f"   Cubic:      mean={statistics.mean(c_tput):.3f} Mbps  Jain={jain(c_tput):.4f}")
    print(f"   FastStart:  mean={statistics.mean(f_tput):.3f} Mbps  Jain={jain(f_tput):.4f}")
    print(f"   Tput change: {(statistics.mean(f_tput)-statistics.mean(c_tput))/statistics.mean(c_tput)*100:+.1f}%")

    # Queue
    c_q = data['cubic_queue']
    f_q = data['fs_queue_110']
    print("\n6. QUEUE OCCUPANCY (bytes, bottleneck buffer = 25000 B)")
    print(f"   Cubic:     mean={statistics.mean(c_q):.0f} B   max={max(c_q)} B")
    print(f"   FastStart: mean={statistics.mean(f_q):.0f} B   max={max(f_q)} B")
    print(f"   Mean queue change: {(statistics.mean(f_q)-statistics.mean(c_q))/statistics.mean(c_q)*100:+.1f}%")
    print("\n" + "="*70)

# ── Main ─────────────────────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(description='Analyse Fast-Start simulation results')
    parser.add_argument('--results_dir', default='./results', help='Directory containing CSV files')
    parser.add_argument('--output_dir',  default='./plots',   help='Directory for output plots')
    args = parser.parse_args()

    os.makedirs(args.output_dir, exist_ok=True)

    print(f"Loading data from: {args.results_dir}")
    data = load_all(args.results_dir)

    print("Generating summary table...")
    print_summary(data, args.results_dir)

    if HAS_MPL:
        print("\nGenerating plots...")
        plot_fct_cdf(data, args.output_dir)
        plot_slowdown(data, args.output_dir)
        plot_retransmit_bar(args.results_dir, args.output_dir)
        plot_threshold_sweep(args.results_dir, args.output_dir)
        plot_queue_cdf(data, args.output_dir)
        plot_elephant_throughput(data, args.output_dir)
        print(f"\nAll plots saved to: {args.output_dir}")
    else:
        print("\nInstall matplotlib to generate plots: pip install matplotlib")

    print("\nDone.")

if __name__ == '__main__':
    main()
