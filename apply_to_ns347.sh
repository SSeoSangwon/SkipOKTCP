#!/usr/bin/env bash

set -euo pipefail

BUNDLE_ROOT="$(cd "$(dirname "$0")" && pwd)"
TARGET_ROOT="${1:-$PWD}"

if [ ! -f "${TARGET_ROOT}/ns3" ] && [ ! -x "${TARGET_ROOT}/ns3" ]; then
    echo "ERROR: target does not look like an ns-3 root: ${TARGET_ROOT}" >&2
    echo "Usage: $0 /path/to/ns-3.47" >&2
    exit 2
fi

if [ ! -d "${TARGET_ROOT}/src/internet/model" ]; then
    echo "ERROR: missing src/internet/model in target: ${TARGET_ROOT}" >&2
    exit 2
fi

STAMP="$(date +%Y%m%d_%H%M%S)"
BACKUP_DIR="${TARGET_ROOT}/compact_tcp_backup_${STAMP}"

mkdir -p "${BACKUP_DIR}/src/internet/model"

CORE_FILES=(
    tcp-tx-item.h
    tcp-tx-item.cc
    tcp-tx-buffer.h
    tcp-tx-buffer.cc
    tcp-rx-buffer.h
    tcp-rx-buffer.cc
    tcp-socket-base.h
    tcp-socket-base.cc
)

for file in "${CORE_FILES[@]}"; do
    cp \
        "${TARGET_ROOT}/src/internet/model/${file}" \
        "${BACKUP_DIR}/src/internet/model/${file}"

done

cp \
    "${BUNDLE_ROOT}/src/internet/model/"* \
    "${TARGET_ROOT}/src/internet/model/"

mkdir -p "${TARGET_ROOT}/scratch"
cp \
    "${BUNDLE_ROOT}/scratch/"*.cc \
    "${TARGET_ROOT}/scratch/"

cp \
    "${BUNDLE_ROOT}/run_all_correctness.sh" \
    "${TARGET_ROOT}/"

cp \
    "${BUNDLE_ROOT}/run_phase11_benchmark.sh" \
    "${BUNDLE_ROOT}/run_phase12_robustness.sh" \
    "${BUNDLE_ROOT}/run_phase13_stock_equivalence.sh" \
    "${BUNDLE_ROOT}/run_phase14_scale.sh" \
    "${TARGET_ROOT}/"

chmod +x \
    "${TARGET_ROOT}/run_all_correctness.sh" \
    "${TARGET_ROOT}/run_phase11_benchmark.sh" \
    "${TARGET_ROOT}/run_phase12_robustness.sh" \
    "${TARGET_ROOT}/run_phase13_stock_equivalence.sh" \
    "${TARGET_ROOT}/run_phase14_scale.sh"

echo "COMPACT_TCP_APPLY=PASS"
echo "TARGET_ROOT=${TARGET_ROOT}"
echo "BACKUP_DIR=${BACKUP_DIR}"
echo
echo "Next:"
echo "  cd ${TARGET_ROOT}"
echo "  ./ns3 build"
echo "  ./run_all_correctness.sh --with-ns3-regression"
