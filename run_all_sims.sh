#!/usr/bin/env bash
# =============================================================================
# run_all_sims.sh — Run all 20 AFS simulation configurations
#
# Authors: Anshuman Dave (231EE107) | Purva Siddapurmath (231EE242)
#
# Runs:
#   5 seeds × 1 Cubic baseline         =  5 runs
#   5 seeds × 4 AFS k values           = 20 runs
#   Total                              = 25 runs
#
# Each run writes results to ./results/
#
# Usage:
#   cd <ns-3-root>
#   chmod +x run_all_sims.sh
#   ./run_all_sims.sh
#
# Prerequisites:
#   1. tcp-afs.h and tcp-afs.cc copied to src/internet/model/
#   2. Both files added to src/internet/CMakeLists.txt (model sources)
#   3. afs_sim.cc copied to scratch/
#   4. ns-3 built: ./ns3 build
# =============================================================================

set -euo pipefail

NS3_ROOT="$(pwd)"
OUTPUT_DIR="${NS3_ROOT}/results"
SEEDS=(1 2 3 4 5)
K_VALUES=(1.5 2.0 2.5 3.0)
DURATION=200
IW_MAX=40
MOUSE_THRESH=200000

mkdir -p "${OUTPUT_DIR}"

total_runs=$(( ${#SEEDS[@]} + ${#SEEDS[@]} * ${#K_VALUES[@]} ))
current=0

echo "========================================================"
echo "AFS Simulation Suite"
echo "Output directory: ${OUTPUT_DIR}"
echo "Total runs: ${total_runs}"
echo "========================================================"

# ── 1. Cubic baseline (5 seeds) ──────────────────────────────────────────────
echo ""
echo "── Running TCP Cubic baseline ──────────────────────────"
for seed in "${SEEDS[@]}"; do
    current=$((current + 1))
    echo "[${current}/${total_runs}] Cubic seed=${seed}"
    ./ns3 run "scratch/afs_sim \
        --cc=cubic \
        --seed=${seed} \
        --duration=${DURATION} \
        --output-dir=${OUTPUT_DIR}" \
        2>&1 | tail -1
done

# ── 2. AFS with k sweeps (5 seeds × 4 k values) ──────────────────────────────
echo ""
echo "── Running AFS with k sweep ────────────────────────────"
for k in "${K_VALUES[@]}"; do
    for seed in "${SEEDS[@]}"; do
        current=$((current + 1))
        echo "[${current}/${total_runs}] AFS seed=${seed} k=${k}"
        ./ns3 run "scratch/afs_sim \
            --cc=afs \
            --seed=${seed} \
            --exit-k=${k} \
            --iw-max=${IW_MAX} \
            --mouse-threshold=${MOUSE_THRESH} \
            --duration=${DURATION} \
            --output-dir=${OUTPUT_DIR}" \
            2>&1 | tail -1
    done
done

echo ""
echo "========================================================"
echo "All ${total_runs} runs complete."
echo "Results written to: ${OUTPUT_DIR}"
echo ""
echo "Next step: python3 analyse_results.py --results-dir ${OUTPUT_DIR} --output-dir ./plots"
echo "========================================================"