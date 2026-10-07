# SkipOKTCP

**Fall 2024 · SKKU URP**

SkipOKTCP is an experimental TCP extension implemented in ns-3. It distinguishes application data by importance and applies different recovery behavior to critical and non-critical data.

## Overview

Standard TCP reliably retransmits lost data. SkipOKTCP explores a partial-reliability model in which the application can mark transmitted data as critical or non-critical.

- Critical data is retransmitted after loss.
- Lost non-critical byte ranges can be explicitly skipped instead of retransmitted.
- Importance information is preserved in TCP transmit metadata.
- A SKIP control mechanism advances the receiver over abandoned non-critical byte ranges.

This implementation is intended for controlled ns-3 experiments and is not wire-compatible with standard TCP implementations.

## Environment

- Language: C++
- Simulator: ns-3
- Base version: ns-3.47
- Area: Computer Networks / Transport Protocols

## Main Implementation

The main TCP modifications are located in:

```text
src/internet/model/
├── tcp-tx-item.cc
├── tcp-tx-item.h
├── tcp-tx-buffer.cc
├── tcp-tx-buffer.h
├── tcp-rx-buffer.cc
├── tcp-rx-buffer.h
├── tcp-socket-base.cc
└── tcp-socket-base.h
```

The implementation includes:

- application-defined critical / non-critical marking
- propagation of importance metadata through the TCP transmit buffer
- reliable retransmission of critical data
- selective skipping of lost non-critical byte ranges
- retransmission of lost SKIP control information
- multiple-loss-hole handling
- duplicate and premature SKIP safety checks
- payload-content and ordering validation

## Tests

The `scratch/` directory contains correctness and behavior tests for the modified TCP implementation, including:

- critical-data marking
- critical-data loss recovery
- basic selective skipping
- SKIP loss and retransmission
- multiple non-critical loss holes
- payload-content correctness
- duplicate SKIP handling
- mixed critical and non-critical loss
- consecutive skipped ranges
- large non-critical transmissions

The full correctness suite can be executed with:

```bash
./run_all_correctness.sh
```

## Evaluation

Evaluation scripts are included for robustness, stock-equivalence, and scale experiments:

```text
run_selective_recovery_benchmark.sh
run_robustness_evaluation.sh
run_stock_equivalence_check.sh
run_scale_evaluation.sh
```

Aggregated result files are stored under `results/`.

## Applying to ns-3.47

The repository includes `apply_to_ns347.sh` for applying the modified source files to an ns-3.47 source tree.

## Project Context

This project was conducted as part of the Sungkyunkwan University Undergraduate Research Program (URP) during the Fall 2024 semester.

This repository was later organized and published on GitHub for archival and portfolio purposes.

## License

See `LICENSE`.
