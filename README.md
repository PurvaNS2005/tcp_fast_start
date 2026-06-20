# Adaptive Fast-Start (AFS) TCP Congestion Control

A sender-side modification to TCP Cubic that speeds up short-flow ("mouse") completion
times during slow start, without harming long-flow ("elephant") throughput or exceeding
a bounded retransmission budget. Implemented as an ns-3 congestion control module and
evaluated against a TCP Cubic baseline on a dumbbell topology with mixed mouse/elephant
traffic.

**Course:** CS301M — Computer Networks
**Authors:** Anshuman Dave (231EE107) · Purva Siddapurmath (231EE242)

---

## How it works

`TcpAdaptiveFastStart` extends ns-3's `TcpCubic` and overrides exactly three places in
the connection lifecycle. Everything else — Cubic's window function, fast retransmit,
fast recovery, loss handling — is left untouched.

| Component | Fires on | What it does |
|---|---|---|
| **C1 — BDP-Adaptive Initial Window** | `CongestionStateSet` (CA_OPEN) | Computes IW from RTT₀ and receiver window instead of using a fixed RFC 6928 value: `IW = min(BDP/MSS, rwnd/(2·MSS), IwMax)` |
| **C2 — Online Flow Classifier** | `IncreaseWindow`, after 2 RTTs | Fits an exponential growth model to ACKed bytes and projects total flow size. Flows projected ≥ `MouseThreshold` (default 200 KB) are classified **Elephant** and immediately handed off to Cubic congestion avoidance |
| **C3 — Statistical RTT Anomaly Detector** | `IncreaseWindow`, every RTT boundary | Maintains a rolling mean (μ) and stddev (σ) of RTT samples. Exits slow start when `RTT_current > μ + k·σ` (default k = 2.0), signaling early congestion before a loss event |

## Repo structure
tcp_afs.cc / tcp_afs.h     — AFS congestion control module (ns-3 src/internet/model/)

afs_sim.cc                 — dumbbell-topology simulation driver (ns-3 scratch/)

afs-dumbbell.cc            — work-in-progress alternate topology script (untracked, WIP)

run_all_sims.sh            — runs the full 25-simulation sweep

analyse_results.py         — parses results/, builds the Section V summary table + plots

results/                   — raw per-run CSVs (FCT, retransmit, queue occupancy)

plots/                     — generated summary table + figures

## Setup

This is an ns-3 module, not a standalone binary — it has to be built inside an ns-3 checkout.

```bash
export NS3=~/ns-allinone-3.40/ns-3.40   # path to your ns-3 build

# 1. Copy the congestion control module in
cp tcp_afs.cc tcp_afs.h "$NS3/src/internet/model/"

# 2. Register the files in src/internet/CMakeLists.txt
#    (add "model/tcp_afs.cc" to sources, "model/tcp_afs.h" to headers)

# 3. Copy the simulation driver into scratch/
cp afs_sim.cc "$NS3/scratch/"

# 4. Build
cd "$NS3" && ./ns3 build
```

## Running the experiments

```bash
cp run_all_sims.sh "$NS3/" && chmod +x "$NS3/run_all_sims.sh"
cd "$NS3" && ./run_all_sims.sh
```

This runs:
- 5 seeds × TCP Cubic baseline
- 5 seeds × 4 values of `k` (1.5, 2.0, 2.5, 3.0) for AFS

25 runs total, ~200s simulated duration each. Output CSVs land in `results/`.

### Single run (manual)
```bash
./ns3 run "scratch/afs_sim --cc=cubic --seed=1 --duration=200"
./ns3 run "scratch/afs_sim --cc=afs --seed=1 --exit-k=2.0 --iw-max=40 --mouse-threshold=200000 --duration=200"
```

## Analysis

```bash
cp -r "$NS3/results" .
python3 analyse_results.py --results-dir ./results --output-dir ./plots
```

Requires `pandas`, `matplotlib`, `numpy` (`pip install pandas matplotlib numpy --break-system-packages` if missing).

## Results summary (k = 2.0, 5 seeds)

| Metric | Cubic | AFS | Change |
|---|---|---|---|
| Mean FCT | 0.207 s | 0.197 s | **-5.0%** |
| Median FCT | 0.132 s | 0.118 s | **-11.0%** |
| P95 FCT | 0.590 s | 0.575 s | -2.4% |
| Retransmission rate | 0.75% | 0.84% | 1.12× (target ≤ 1.5×) |
| Elephant throughput | 1.728 Mbps | 1.840 Mbps | **+6.5%** |
| Mean queue occupancy | 24 B | 27 B | +12.2% (buffer = 25,000 B) |

Short flows in the 20–100 KB range see the largest FCT improvement (-9.8% to -15.9%),
consistent with that being the window where a larger adaptive IW matters most without
triggering the elephant classifier. Full breakdown by flow-size bucket and the
generated figures are in `plots/`.

**Note:** the `k` sweep (1.5–3.0) did not produce a measurable difference in
retransmission rate on the runs collected so far — sensitivity analysis across `k`
was outside this project's scope, so this is reported as-is rather than investigated
further.

## Safety properties verified

- AFS never increases retransmissions beyond 1.5× the Cubic baseline
- Elephant flows are detected within 2 RTTs and handed off to standard Cubic CA
- Bottleneck queue occupancy stays well within buffer capacity (max 124 B / 25,000 B)
