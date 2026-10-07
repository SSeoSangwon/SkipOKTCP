#!/usr/bin/env bash

set -u

cd "$(dirname "$0")"

OUTDIR="results/phase14"
RAW="${OUTDIR}/phase14_raw.txt"

mkdir -p "${OUTDIR}"
: > "${RAW}"

RUN_FAILURE=0

# 2000 records x 400 B = 800,000 application bytes per run.
RECORDS=2000
RECORD_SIZE=400

for LOSS in 0.03 0.06; do
    for CRITICAL in 25 50 75; do
        for SEED in $(seq 1 10); do
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
                    --seed=${SEED} \
                    --records=${RECORDS} \
                    --recordSize=${RECORD_SIZE}" \
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

outdir = Path("results/phase14")
raw_path = outdir / "phase14_raw.txt"
csv_path = outdir / "phase14_results.csv"

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
    writer.writerows(rows)


def exact_sign_test(wins, losses):
    n = wins + losses

    if n == 0:
        return 1.0

    k = min(wins, losses)

    tail = sum(
        math.comb(n, i)
        for i in range(k + 1)
    )

    return min(
        1.0,
        2.0 * tail / (2 ** n),
    )


expected_runs = 2 * 3 * 10 * 2

all_present = (
    len(rows) == expected_runs
)

all_valid = (
    all_present
    and all(
        int(row["valid"]) == 1
        for row in rows
    )
)

critical_delivery = (
    all_present
    and all(
        int(row["critical_received"])
        ==
        int(row["critical_expected"])
        for row in rows
    )
)

no_noncritical_retx = all(
    int(row["retx_noncritical"]) == 0
    for row in rows
    if row["mode"] == "selective"
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


paired_loss = True

for pair in pairs.values():

    if (
        "baseline" not in pair
        or "selective" not in pair
    ):
        paired_loss = False
        continue

    for field in (
        "dropped_segments",
        "dropped_bytes",
        "dropped_critical",
        "dropped_noncritical",
    ):
        if (
            pair["baseline"][field]
            != pair["selective"][field]
        ):
            paired_loss = False


print()
print(
    "========== SCALE-UP PAIRED SUMMARY =========="
)

all_cells_latency_improved = True
all_cells_retx_reduced = True

for loss in (0.03, 0.06):
    for critical in (25, 50, 75):

        cell = []

        for key, pair in pairs.items():

            if (
                abs(key[0] - loss) < 1e-12
                and key[1] == critical
                and "baseline" in pair
                and "selective" in pair
            ):
                cell.append(
                    (
                        key[2],
                        pair["baseline"],
                        pair["selective"],
                    )
                )

        cell.sort(key=lambda item: item[0])

        if not cell:
            all_cells_latency_improved = False
            all_cells_retx_reduced = False

            print(
                "SCALE_SUMMARY"
                f" loss={loss:.2f}"
                f" critical={critical}%"
                " n=0"
                " status=NO_DATA"
            )

            continue

        latency_delta = []
        retx_reduction = []

        wins = 0
        losses = 0
        ties = 0

        for seed, baseline, selective in cell:

            base_ms = float(
                baseline[
                    "critical_completion_ms"
                ]
            )

            sel_ms = float(
                selective[
                    "critical_completion_ms"
                ]
            )

            delta = (
                (sel_ms - base_ms)
                / base_ms
                * 100.0
            )

            latency_delta.append(delta)

            if delta < -1e-12:
                wins += 1
            elif delta > 1e-12:
                losses += 1
            else:
                ties += 1

            base_retx = int(
                baseline["retx_bytes"]
            )

            sel_retx = int(
                selective["retx_bytes"]
            )

            if base_retx > 0:
                retx_reduction.append(
                    (
                        base_retx
                        - sel_retx
                    )
                    / base_retx
                    * 100.0
                )

        mean_latency = statistics.mean(
            latency_delta
        )

        median_latency = statistics.median(
            latency_delta
        )

        if retx_reduction:
            mean_retx = statistics.mean(
                retx_reduction
            )

            median_retx = statistics.median(
                retx_reduction
            )
        else:
            mean_retx = 0.0
            median_retx = 0.0
            all_cells_retx_reduced = False

        p = exact_sign_test(
            wins,
            losses,
        )

        if mean_latency >= 0.0:
            all_cells_latency_improved = False

        if mean_retx <= 0.0:
            all_cells_retx_reduced = False

        print(
            "SCALE_SUMMARY"
            f" loss={loss:.2f}"
            f" critical={critical}%"
            f" n={len(cell)}"
            f" latency_mean_delta_pct={mean_latency:.4f}"
            f" latency_median_delta_pct={median_latency:.4f}"
            f" wins={wins}"
            f" losses={losses}"
            f" ties={ties}"
            f" sign_test_p={p:.6f}"
            f" retx_mean_reduction_pct={mean_retx:.3f}"
            f" retx_median_reduction_pct={median_retx:.3f}"
        )


overall = (
    all_present
    and all_valid
    and paired_loss
    and critical_delivery
    and no_noncritical_retx
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
    + ("PASS" if all_valid else "FAIL")
)

print(
    "PAIRED_LOSS_PATTERN="
    + ("PASS" if paired_loss else "FAIL")
)

print(
    "CRITICAL_DELIVERY="
    + ("PASS" if critical_delivery else "FAIL")
)

print(
    "SELECTIVE_NONCRITICAL_DATA_RETX="
    + (
        "PASS"
        if no_noncritical_retx
        else "FAIL"
    )
)

print(
    "LATENCY_DIRECTION_ALL_CELLS="
    + (
        "PASS"
        if all_cells_latency_improved
        else "REVIEW"
    )
)

print(
    "RETX_REDUCTION_ALL_CELLS="
    + (
        "PASS"
        if all_cells_retx_reduced
        else "REVIEW"
    )
)

print(
    "PHASE14_SCALE_VALIDATION="
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
