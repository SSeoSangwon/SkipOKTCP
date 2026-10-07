#!/usr/bin/env bash

set -u

cd "$(dirname "$0")"

OUTDIR="results/benchmark"
RAW="${OUTDIR}/benchmark_raw.txt"

mkdir -p "${OUTDIR}"
: > "${RAW}"

RUN_FAILURE=0

for LOSS in 0.03 0.06; do
    for CRITICAL in 25 50 75; do
        for SEED in 1 2 3; do
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
                    printf '%s\n' "${OUTPUT}" | tail -n 20
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
import statistics

outdir = Path("results/benchmark")
raw_path = outdir / "benchmark_raw.txt"
csv_path = outdir / "selective_recovery_benchmark.csv"

rows = []

for line in raw_path.read_text().splitlines():
    line = line.strip()

    if not line.startswith("RESULT "):
        continue

    values = {}

    for token in line.split()[1:]:
        key, value = token.split("=", 1)
        values[key] = value

    rows.append(values)

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

expected_runs = 36

all_runs_present = len(rows) == expected_runs

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
        == int(row["critical_expected"])
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
        row["loss"],
        row["critical"],
        row["seed"],
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
        "dropped_bytes",
        "dropped_critical",
        "dropped_noncritical",
    ):
        if baseline[field] != selective[field]:
            pairing_pass = False

print()
print("========== PAIRED PERFORMANCE SUMMARY ==========")

for loss in ("0.030000", "0.060000"):
    for critical in ("25", "50", "75"):

        selected_pairs = []

        for key, pair in pairs.items():
            if (
                key[0] == loss
                and key[1] == critical
                and "baseline" in pair
                and "selective" in pair
            ):
                selected_pairs.append(pair)

        if not selected_pairs:
            continue

        base_critical = [
            float(p["baseline"][
                "critical_completion_ms"
            ])
            for p in selected_pairs
        ]

        selective_critical = [
            float(p["selective"][
                "critical_completion_ms"
            ])
            for p in selected_pairs
        ]

        base_retx = [
            int(p["baseline"]["retx_bytes"])
            for p in selected_pairs
        ]

        selective_retx = [
            int(p["selective"]["retx_bytes"])
            for p in selected_pairs
        ]

        skipped = [
            int(p["selective"][
                "dropped_noncritical"
            ])
            for p in selected_pairs
        ]

        base_ms = statistics.mean(
            base_critical
        )

        selective_ms = statistics.mean(
            selective_critical
        )

        if base_ms > 0:
            critical_delta_pct = (
                (selective_ms - base_ms)
                / base_ms
                * 100.0
            )
        else:
            critical_delta_pct = 0.0

        base_retx_mean = statistics.mean(
            base_retx
        )

        selective_retx_mean = statistics.mean(
            selective_retx
        )

        if base_retx_mean > 0:
            retx_reduction_pct = (
                (base_retx_mean -
                 selective_retx_mean)
                / base_retx_mean
                * 100.0
            )
        else:
            retx_reduction_pct = 0.0

        print(
            "PAIR_SUMMARY"
            f" loss={float(loss):.2f}"
            f" critical={critical}%"
            f" n={len(selected_pairs)}"
            f" baseline_critical_ms={base_ms:.3f}"
            f" selective_critical_ms={selective_ms:.3f}"
            f" critical_delta_pct={critical_delta_pct:.3f}"
            f" baseline_retx_bytes={base_retx_mean:.1f}"
            f" selective_retx_bytes={selective_retx_mean:.1f}"
            f" retx_reduction_pct={retx_reduction_pct:.3f}"
            f" abandoned_noncritical_bytes={statistics.mean(skipped):.1f}"
        )

overall = (
    all_runs_present
    and all_valid
    and critical_delivery
    and selective_no_noncritical_retx
    and pairing_pass
)

print()
print("================ FINAL SUMMARY ================")
print(
    f"RUN_COUNT={len(rows)}/{expected_runs}"
)

print(
    "ALL_RUNS_VALID="
    + ("PASS" if all_valid else "FAIL")
)

print(
    "PAIRED_LOSS_PATTERN="
    + ("PASS" if pairing_pass else "FAIL")
)

print(
    "CRITICAL_DELIVERY="
    + ("PASS" if critical_delivery else "FAIL")
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
    "RESULT_CSV_CREATED="
    + ("PASS" if csv_path.exists() else "FAIL")
)

print(
    "PHASE11_CHARACTERIZATION="
    + ("PASS" if overall else "REVIEW")
)

print(
    f"RESULT_CSV={csv_path}"
)

print("===============================================")
PY

exit "${RUN_FAILURE}"
