#!/usr/bin/env bash

set -u

cd "$(dirname "$0")"

OUTDIR="results/phase12"
RAW="${OUTDIR}/phase12_raw.txt"

mkdir -p "${OUTDIR}"
: > "${RAW}"

RUN_FAILURE=0

for LOSS in 0.03 0.06; do
    for CRITICAL in 25 50 75; do
        for SEED in $(seq 1 20); do
            for SELECTIVE in 0 1; do

                if [ "${SELECTIVE}" -eq 0 ]; then
                    MODE="baseline"
                else
                    MODE="selective"
                fi

                echo \
                    "RUN loss=${LOSS} critical=${CRITICAL} seed=${SEED} mode=${MODE}"

                OUTPUT="$(
                    ./ns3 run \
                    "scratch/selective-recovery-benchmark \
                    --selective=${SELECTIVE} \
                    --lossRate=${LOSS} \
                    --criticalPercent=${CRITICAL} \
                    --seed=${SEED}" \
                    2>&1
                )"

                RC=$?

                RESULT_LINE="$(
                    printf '%s\n' "${OUTPUT}" |
                    grep '^RESULT ' |
                    tail -n 1
                )"

                if [ -z "${RESULT_LINE}" ]; then
                    echo \
                        "RUN_ERROR loss=${LOSS} critical=${CRITICAL} seed=${SEED} mode=${MODE}"

                    printf '%s\n' "${OUTPUT}" |
                        tail -n 20

                    RUN_FAILURE=1
                    continue
                fi

                echo "${RESULT_LINE}"
                echo "${RESULT_LINE}" >> "${RAW}"

                if [ "${RC}" -ne 0 ]; then
                    RUN_FAILURE=1
                fi
            done
        done
    done
done

python3 <<'PY'
from pathlib import Path
import csv
import math
import statistics

outdir = Path("results/phase12")
raw_path = outdir / "phase12_raw.txt"
csv_path = outdir / "phase12_results.csv"

rows = []

for line in raw_path.read_text().splitlines():
    line = line.strip()

    if not line.startswith("RESULT "):
        continue

    row = {}

    for token in line.split()[1:]:
        key, value = token.split("=", 1)
        row[key] = value

    rows.append(row)

fields = [
    "mode",
    "loss",
    "critical",
    "seed",
    "sent",
    "received",
    "critical_expected",
    "critical_received",
    "dropped_segments",
    "dropped_bytes",
    "dropped_critical",
    "dropped_noncritical",
    "retx_bytes",
    "retx_critical",
    "retx_noncritical",
    "skip_bytes",
    "skip_controls",
    "critical_completion_ms",
    "last_delivery_ms",
    "critical_goodput_mbps",
    "delivered_goodput_mbps",
    "unexpected",
    "valid",
]

with csv_path.open("w", newline="") as f:
    writer = csv.DictWriter(
        f,
        fieldnames=fields,
    )

    writer.writeheader()

    for row in rows:
        writer.writerow(row)


def exact_sign_test(wins, losses):
    n = wins + losses

    if n == 0:
        return 1.0

    k = min(wins, losses)

    cumulative = sum(
        math.comb(n, i)
        for i in range(k + 1)
    )

    p = (
        2.0 *
        cumulative /
        (2 ** n)
    )

    return min(1.0, p)


expected_runs = (
    2 *    # loss rates
    3 *    # critical ratios
    20 *   # seeds
    2      # modes
)

all_runs_present = (
    len(rows) == expected_runs
)

all_valid = (
    all_runs_present
    and all(
        int(row["valid"]) == 1
        for row in rows
    )
)

critical_delivery = (
    all_runs_present
    and all(
        int(row["critical_received"])
        ==
        int(row["critical_expected"])
        for row in rows
    )
)

selective_no_noncritical_retx = (
    all(
        int(row["retx_noncritical"]) == 0
        for row in rows
        if row["mode"] == "selective"
    )
)

pairs = {}

for row in rows:
    key = (
        float(row["loss"]),
        int(row["critical"]),
        int(row["seed"]),
    )

    pairs.setdefault(key, {})[
        row["mode"]
    ] = row

pairing_pass = True

for key, pair in pairs.items():
    if (
        "baseline" not in pair
        or "selective" not in pair
    ):
        pairing_pass = False
        continue

    baseline = pair["baseline"]
    selective = pair["selective"]

    for field in (
        "dropped_segments",
        "dropped_bytes",
        "dropped_critical",
        "dropped_noncritical",
    ):
        if baseline[field] != selective[field]:
            pairing_pass = False


print()
print(
    "========== ROBUST PAIRED SUMMARY =========="
)

cell_mean_latency_improved = []

for loss in (0.03, 0.06):
    for critical in (25, 50, 75):

        cell_pairs = []

        for key, pair in pairs.items():
            if (
                abs(key[0] - loss) < 1e-12
                and key[1] == critical
                and "baseline" in pair
                and "selective" in pair
            ):
                cell_pairs.append(
                    (
                        key[2],
                        pair["baseline"],
                        pair["selective"],
                    )
                )

        cell_pairs.sort(
            key=lambda item: item[0]
        )

        latency_deltas = []
        retx_reductions = []

        latency_wins = 0
        latency_losses = 0
        latency_ties = 0

        for seed, baseline, selective in cell_pairs:

            baseline_ms = float(
                baseline[
                    "critical_completion_ms"
                ]
            )

            selective_ms = float(
                selective[
                    "critical_completion_ms"
                ]
            )

            if (
                baseline_ms > 0.0
                and selective_ms > 0.0
            ):
                delta_pct = (
                    (selective_ms - baseline_ms)
                    /
                    baseline_ms
                    *
                    100.0
                )

                latency_deltas.append(
                    delta_pct
                )

                if delta_pct < -1e-12:
                    latency_wins += 1
                elif delta_pct > 1e-12:
                    latency_losses += 1
                else:
                    latency_ties += 1

            baseline_retx = int(
                baseline["retx_bytes"]
            )

            selective_retx = int(
                selective["retx_bytes"]
            )

            if baseline_retx > 0:
                reduction_pct = (
                    (baseline_retx -
                     selective_retx)
                    /
                    baseline_retx
                    *
                    100.0
                )

                retx_reductions.append(
                    reduction_pct
                )

        if latency_deltas:
            mean_latency_delta = (
                statistics.mean(
                    latency_deltas
                )
            )

            median_latency_delta = (
                statistics.median(
                    latency_deltas
                )
            )

            min_latency_delta = min(
                latency_deltas
            )

            max_latency_delta = max(
                latency_deltas
            )
        else:
            mean_latency_delta = 0.0
            median_latency_delta = 0.0
            min_latency_delta = 0.0
            max_latency_delta = 0.0

        if retx_reductions:
            mean_retx_reduction = (
                statistics.mean(
                    retx_reductions
                )
            )

            median_retx_reduction = (
                statistics.median(
                    retx_reductions
                )
            )
        else:
            mean_retx_reduction = 0.0
            median_retx_reduction = 0.0

        sign_p = exact_sign_test(
            latency_wins,
            latency_losses,
        )

        improved = (
            mean_latency_delta < 0.0
        )

        cell_mean_latency_improved.append(
            improved
        )

        print(
            "ROBUST_SUMMARY"
            f" loss={loss:.2f}"
            f" critical={critical}%"
            f" n={len(cell_pairs)}"
            f" latency_mean_delta_pct="
            f"{mean_latency_delta:.4f}"
            f" latency_median_delta_pct="
            f"{median_latency_delta:.4f}"
            f" latency_min_delta_pct="
            f"{min_latency_delta:.4f}"
            f" latency_max_delta_pct="
            f"{max_latency_delta:.4f}"
            f" wins={latency_wins}"
            f" losses={latency_losses}"
            f" ties={latency_ties}"
            f" sign_test_p="
            f"{sign_p:.6f}"
            f" retx_mean_reduction_pct="
            f"{mean_retx_reduction:.3f}"
            f" retx_median_reduction_pct="
            f"{median_retx_reduction:.3f}"
        )


latency_direction_all_cells = (
    len(cell_mean_latency_improved) == 6
    and all(
        cell_mean_latency_improved
    )
)

overall = (
    all_runs_present
    and all_valid
    and pairing_pass
    and critical_delivery
    and selective_no_noncritical_retx
)

print()
print(
    "================ FINAL SUMMARY ================"
)

print(
    f"RUN_COUNT={len(rows)}/{expected_runs}"
)

print(
    "ALL_RUNS_VALID="
    + (
        "PASS"
        if all_valid
        else "FAIL"
    )
)

print(
    "PAIRED_LOSS_PATTERN="
    + (
        "PASS"
        if pairing_pass
        else "FAIL"
    )
)

print(
    "CRITICAL_DELIVERY="
    + (
        "PASS"
        if critical_delivery
        else "FAIL"
    )
)

print(
    "SELECTIVE_NONCRITICAL_DATA_RETX="
    + (
        "PASS"
        if selective_no_noncritical_retx
        else "FAIL"
    )
)

print(
    "LATENCY_DIRECTION_ALL_CELLS="
    + (
        "PASS"
        if latency_direction_all_cells
        else "REVIEW"
    )
)

print(
    "RESULT_CSV_CREATED="
    + (
        "PASS"
        if csv_path.exists()
        else "FAIL"
    )
)

print(
    "PHASE12_CHARACTERIZATION="
    + (
        "PASS"
        if overall
        else "REVIEW"
    )
)

print(
    f"RESULT_CSV={csv_path}"
)

print(
    "==============================================="
)
PY

exit "${RUN_FAILURE}"
