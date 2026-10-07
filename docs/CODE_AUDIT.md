# Compact TCP code audit

## Core behavior

The implementation is an experimental, non-standard extension of ns-3.47 TCP.

- Application `Send(..., flags)` bit 0 marks a byte range as critical.
- `TcpTxItem` stores the application-defined criticality bit.
- `TcpTxBuffer` preserves that bit across splitting and avoids merging ranges of different criticality.
- Critical DATA is emitted with TCP URG set.
- With selective recovery enabled, retransmission of a lost non-critical range is replaced by a zero-payload `ACK|URG` SKIP control.
- The SKIP sequence number denotes the first abandoned byte; the urgent pointer carries the abandoned byte count.
- The receiver applies a SKIP only when its sequence number equals `NextRxSequence()`.
- Duplicate and premature SKIPs do not advance receive state; the receiver replies with its current cumulative ACK.
- SKIP is retransmitted on an RTO-based timer until a cumulative ACK covers the abandoned range.
- The receive buffer promotes already buffered following bytes after a hole is abandoned.

## Semantics

The design provides byte-range partial reliability, not message atomicity.

If a 3000-byte non-critical application send is segmented and one 536-byte TCP range is lost, only the lost 536-byte range is abandoned. Successfully received bytes from the same application send remain deliverable.

Applications that need message boundaries or whole-message discard require framing above this transport.

## Verified evidence from the completed experiments

The supplied source corresponds to the implementation used for the reported project runs.

- Importance propagation and URG marking: PASS.
- Critical loss retransmission: PASS.
- Non-critical loss replacement by SKIP: PASS.
- SKIP loss and retransmission: PASS.
- Multiple non-critical holes: PASS.
- Payload content/order after SKIP: PASS.
- Premature/out-of-order SKIP safety: PASS in the bounded multi-hole tests.
- Duplicate SKIP safety under forced ACK suppression: PASS.
- Mixed critical/non-critical recovery: PASS.
- Consecutive 200-byte non-critical range abandonment: PASS.
- Large-send byte-range semantics: confirmed.
- Phase 12 robustness: 240/240 valid runs; critical delivery preserved; all six cells had negative mean critical-completion deltas.
- Phase 13 stock equivalence: 40/40 valid; simulated completion identical with the feature disabled in the tested stock workload.
- Phase 14 scale validation: 120/120 valid; all six cells reduced retransmission and had negative mean critical-completion deltas.

## Important limitations

1. This is not wire-compatible standard TCP semantics. URG and the urgent pointer are repurposed experimentally.
2. The sender stores one explicit pending SKIP timer state (`m_skipSeq`, `m_skipBytes`). Bounded multi-hole tests pass because the ordinary loss-recovery machinery can regenerate SKIPs, but this is not a general proof of arbitrary numbers of simultaneously outstanding SKIP controls.
3. A single SKIP length is limited to 65535 bytes by the 16-bit urgent-pointer field. The implementation aborts if a requested SKIP exceeds that value.
4. SKIP controls bypass normal DATA congestion-control/rate accounting. The current project changes recovery behavior, not TCP congestion-control semantics.
5. Loss of non-critical data can still trigger ordinary TCP congestion responses before that range is abandoned.
6. The receiver does not expose an application-level "bytes skipped" event. It presents the shortened byte stream after the abandoned range is removed.
7. SKIP recognition is currently limited to the ESTABLISHED state.
8. The project validates ns-3 behavior; it is not a production kernel TCP implementation.

## Code-quality cleanup in this release bundle

Core behavior was intentionally left unchanged. Only non-functional comments/alignment were cleaned in `tcp-socket-base.{h,cc}`. The legacy critical-loss diagnostic was replaced by a self-checking test that drops the first critical DATA segment by protocol contents rather than a fragile packet index.
