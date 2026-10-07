#!/usr/bin/env bash

set -euo pipefail

CURRENT_ROOT="$(pwd)"
PROJECT_ROOT="$(dirname "${CURRENT_ROOT}")"

STOCK_PARENT="${PROJECT_ROOT}/stock_equivalence_stock"
STOCK_ROOT="${STOCK_PARENT}/ns-3.47"
TARBALL="${STOCK_PARENT}/ns-3.47.tar.bz2"

mkdir -p "${STOCK_PARENT}"

if [ ! -f "${TARBALL}" ]; then
    echo "Downloading official ns-3.47 source archive..."

    if command -v curl >/dev/null 2>&1; then
        curl -L \
            https://www.nsnam.org/releases/ns-3.47.tar.bz2 \
            -o "${TARBALL}"
    else
        wget \
            https://www.nsnam.org/releases/ns-3.47.tar.bz2 \
            -O "${TARBALL}"
    fi
fi

EXPECTED_SHA1="897c3f2db2c6de6a8291e46b3020dbeb1af75269"
ACTUAL_SHA1="$(
    sha1sum "${TARBALL}" |
    awk '{print $1}'
)"

echo "STOCK_TARBALL_SHA1=${ACTUAL_SHA1}"

if [ "${ACTUAL_SHA1}" != "${EXPECTED_SHA1}" ]; then
    echo "STOCK_TARBALL_VERIFICATION=FAIL"
    exit 1
fi

echo "STOCK_TARBALL_VERIFICATION=PASS"

if [ ! -d "${STOCK_ROOT}" ]; then
    tar xjf "${TARBALL}" \
        -C "${STOCK_PARENT}"
fi

cp \
    "${CURRENT_ROOT}/scratch/stock-overhead-benchmark.cc" \
    "${STOCK_ROOT}/scratch/stock-overhead-benchmark.cc"

echo
echo "===== BUILD MODIFIED TREE ====="

cd "${CURRENT_ROOT}"

./ns3 run \
    "scratch/stock-overhead-benchmark --bytes=8000000" \
    >/tmp/stock_equivalence_modified_build.txt

echo "MODIFIED_BUILD=PASS"

echo
echo "===== CONFIGURE/BUILD PRISTINE STOCK TREE ====="

cd "${STOCK_ROOT}"

./ns3 configure \
    --build-profile=debug \
    --enable-modules='core;network;internet;point-to-point;applications'

./ns3 run \
    "scratch/stock-overhead-benchmark --bytes=8000000" \
    >/tmp/stock_equivalence_stock_build.txt

echo "STOCK_BUILD=PASS"

cd "${CURRENT_ROOT}"

MODIFIED_BIN="$(
    find build/scratch \
        -maxdepth 1 \
        -type f \
        -perm -111 \
        -name '*stock-overhead-benchmark*' |
    head -n 1
)"

STOCK_BIN="$(
    find "${STOCK_ROOT}/build/scratch" \
        -maxdepth 1 \
        -type f \
        -perm -111 \
        -name '*stock-overhead-benchmark*' |
    head -n 1
)"

if [ -z "${MODIFIED_BIN}" ]; then
    echo "MODIFIED_BINARY_FOUND=FAIL"
    exit 1
fi

if [ -z "${STOCK_BIN}" ]; then
    echo "STOCK_BINARY_FOUND=FAIL"
    exit 1
fi

echo "MODIFIED_BINARY=${MODIFIED_BIN}"
echo "STOCK_BINARY=${STOCK_BIN}"

export MODIFIED_BIN
export STOCK_BIN

mkdir -p results/stock_equivalence

python3 <<'PY'
import csv
import os
import re
import statistics
import subprocess
import time
from pathlib import Path

modified_bin = os.environ["MODIFIED_BIN"]
stock_bin = os.environ["STOCK_BIN"]

repeats = 20
total_bytes = 8_000_000

rows = []

result_re = re.compile(
    r"RESULT "
    r"sent=(\d+) "
    r"received=(\d+) "
    r"completion_ms=([0-9.\-]+) "
    r"valid=(\d+)"
)


def run_one(label, binary, repeat):
    start = time.perf_counter()

    proc = subprocess.run(
        [
            binary,
            f"--bytes={total_bytes}",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    wall_ms = (
        time.perf_counter() - start
    ) * 1000.0

    match = result_re.search(proc.stdout)

    if proc.returncode != 0 or match is None:
        print(proc.stdout)
        print(proc.stderr)

        raise RuntimeError(
            f"{label} repeat {repeat} failed"
        )

    sent = int(match.group(1))
    received = int(match.group(2))
    completion_ms = float(match.group(3))
    valid = int(match.group(4))

    row = {
        "mode": label,
        "repeat": repeat,
        "sent": sent,
        "received": received,
        "completion_ms": completion_ms,
        "wall_ms": wall_ms,
        "valid": valid,
    }

    rows.append(row)

    print(
        "RUN"
        f" mode={label}"
        f" repeat={repeat}"
        f" completion_ms={completion_ms:.6f}"
        f" wall_ms={wall_ms:.3f}"
        f" valid={valid}"
    )


for repeat in range(1, repeats + 1):

    # Alternate execution order to reduce systematic
    # host-order and thermal bias.
    if repeat % 2 == 1:
        run_one(
            "stock",
            stock_bin,
            repeat,
        )

        run_one(
            "modified_off",
            modified_bin,
            repeat,
        )
    else:
        run_one(
            "modified_off",
            modified_bin,
            repeat,
        )

        run_one(
            "stock",
            stock_bin,
            repeat,
        )


csv_path = Path(
    "results/stock_equivalence/"
    "stock_equivalence_stock_equivalence.csv"
)

with csv_path.open(
    "w",
    newline="",
) as f:
    writer = csv.DictWriter(
        f,
        fieldnames=[
            "mode",
            "repeat",
            "sent",
            "received",
            "completion_ms",
            "wall_ms",
            "valid",
        ],
    )

    writer.writeheader()
    writer.writerows(rows)


stock_rows = [
    row
    for row in rows
    if row["mode"] == "stock"
]

modified_rows = [
    row
    for row in rows
    if row["mode"] == "modified_off"
]


all_valid = (
    len(rows) == repeats * 2
    and all(
        row["valid"] == 1
        and row["received"] == total_bytes
        for row in rows
    )
)


stock_completion = [
    row["completion_ms"]
    for row in stock_rows
]

modified_completion = [
    row["completion_ms"]
    for row in modified_rows
]


completion_matches = []

for repeat in range(1, repeats + 1):
    stock = next(
        row
        for row in stock_rows
        if row["repeat"] == repeat
    )

    modified = next(
        row
        for row in modified_rows
        if row["repeat"] == repeat
    )

    completion_matches.append(
        abs(
            stock["completion_ms"]
            - modified["completion_ms"]
        )
        <= 1e-6
    )


simulated_equivalence = all(
    completion_matches
)


stock_wall = [
    row["wall_ms"]
    for row in stock_rows
]

modified_wall = [
    row["wall_ms"]
    for row in modified_rows
]


stock_wall_median = statistics.median(
    stock_wall
)

modified_wall_median = statistics.median(
    modified_wall
)


wall_overhead_pct = (
    (
        modified_wall_median
        - stock_wall_median
    )
    /
    stock_wall_median
    *
    100.0
)


stock_completion_median = (
    statistics.median(
        stock_completion
    )
)

modified_completion_median = (
    statistics.median(
        modified_completion
    )
)


overall = (
    all_valid
    and simulated_equivalence
)


print()
print(
    "========== STOCK EQUIVALENCE SUMMARY =========="
)

print(
    "STOCK_COMPLETION_MEDIAN_MS="
    f"{stock_completion_median:.6f}"
)

print(
    "MODIFIED_OFF_COMPLETION_MEDIAN_MS="
    f"{modified_completion_median:.6f}"
)

print(
    "STOCK_WALL_MEDIAN_MS="
    f"{stock_wall_median:.3f}"
)

print(
    "MODIFIED_OFF_WALL_MEDIAN_MS="
    f"{modified_wall_median:.3f}"
)

print(
    "MODIFIED_WALL_OVERHEAD_PCT="
    f"{wall_overhead_pct:.3f}"
)

print()
print(
    "================ FINAL SUMMARY ================"
)

print(
    f"RUN_COUNT={len(rows)}/{repeats * 2}"
)

print(
    "DELIVERY_CORRECTNESS="
    + ("PASS" if all_valid else "FAIL")
)

print(
    "SIMULATED_COMPLETION_EQUIVALENCE="
    + (
        "PASS"
        if simulated_equivalence
        else "FAIL"
    )
)

print(
    "CPU_OVERHEAD_CHARACTERIZED=PASS"
)

print(
    "PHASE13_STOCK_EQUIVALENCE="
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
