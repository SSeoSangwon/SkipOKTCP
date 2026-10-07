#!/usr/bin/env bash

set -u

cd "$(dirname "$0")"

WITH_REGRESSION=0

if [ "${1:-}" = "--with-ns3-regression" ]; then
    WITH_REGRESSION=1
elif [ $# -gt 0 ]; then
    echo "Usage: $0 [--with-ns3-regression]" >&2
    exit 2
fi

OUTDIR="results/final_correctness"
mkdir -p "${OUTDIR}"

OVERALL=0
NAMES=()
STATUSES=()

run_case() {
    local name="$1"
    shift

    local logfile="${OUTDIR}/${name}.log"

    echo
    echo "===== ${name} ====="

    if "$@" >"${logfile}" 2>&1; then
        cat "${logfile}"
        NAMES+=("${name}")
        STATUSES+=("PASS")
    else
        local rc=$?
        cat "${logfile}"
        echo "CASE_EXIT_CODE=${rc}"
        NAMES+=("${name}")
        STATUSES+=("FAIL")
        OVERALL=1
    fi
}

run_case \
    "CRITICAL_URG" \
    ./ns3 run scratch/critical-urg-test

run_case \
    "CRITICAL_LOSS" \
    ./ns3 run scratch/critical-loss-test

run_case \
    "BASIC_SELECTIVE_SKIP" \
    ./ns3 run scratch/selective-skip-test

run_case \
    "SKIP_LOSS_RETRANSMISSION" \
    ./ns3 run scratch/selective-skip-loss-test

run_case \
    "MULTI_HOLE" \
    ./ns3 run "scratch/selective-multi-skip-test --dropFirstSkip=0"

run_case \
    "MULTI_HOLE_WITH_SKIP_LOSS" \
    ./ns3 run "scratch/selective-multi-skip-test --dropFirstSkip=1"

run_case \
    "PAYLOAD_CONTENT" \
    ./ns3 run "scratch/selective-content-test --dropFirstSkip=0"

run_case \
    "PAYLOAD_CONTENT_WITH_SKIP_LOSS" \
    ./ns3 run "scratch/selective-content-test --dropFirstSkip=1"

run_case \
    "DUPLICATE_SKIP" \
    ./ns3 run scratch/selective-duplicate-skip-test

run_case \
    "MIXED_CRITICAL_NONCRITICAL_LOSS" \
    ./ns3 run scratch/selective-mixed-loss-test

run_case \
    "CONSECUTIVE_RANGE" \
    ./ns3 run "scratch/selective-consecutive-skip-test --dropFirstSkip=0"

run_case \
    "CONSECUTIVE_RANGE_WITH_SKIP_LOSS" \
    ./ns3 run "scratch/selective-consecutive-skip-test --dropFirstSkip=1"

run_case \
    "LARGE_SEND_BYTE_RANGE_SEMANTICS" \
    ./ns3 run scratch/selective-large-message-test

if [ "${WITH_REGRESSION}" -eq 1 ]; then
    run_case \
        "NS3_REGRESSION" \
        ./test.py
else
    NAMES+=("NS3_REGRESSION")
    STATUSES+=("SKIPPED")
fi

echo
echo "================ FINAL SUMMARY ================"

for i in "${!NAMES[@]}"; do
    echo "${NAMES[$i]}=${STATUSES[$i]}"
done

if [ "${OVERALL}" -eq 0 ]; then
    echo "OVERALL=PASS"
else
    echo "OVERALL=FAIL"
fi

echo "LOG_DIR=${OUTDIR}"
echo "==============================================="

exit "${OVERALL}"
