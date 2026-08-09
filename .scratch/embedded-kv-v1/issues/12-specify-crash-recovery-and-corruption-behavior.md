# Specify crash recovery and corruption behavior

Type: grilling
Status: resolved
Blocked by: 03, 04, 05, 07

## Question

For every interruption point and durable file state, what must open and recovery do; how are incomplete commits distinguished from corruption or authentication failure; what data-loss boundary is promised; and when must the library fail closed rather than repair, salvage, or continue read-only?

## Resolution

Resolved by the frozen [recovery, maintenance, and verification contract](../../../docs/recovery-maintenance-verification-contract.md) and indexed by [implementation ticket 03](../../embedded-kv-v1-implementation/issues/03-freeze-recovery-maintenance-and-verification.md).

Open is byte-preserving: it authenticates the dual publication slots, may select the predecessor only when the newer designated slot is incomplete or unauthenticated, and never falls back beneath an authenticated publication whose reachable state is corrupt. Transaction failures before the first publication-slot write are `CommitFailed`; failures from that write until the second barrier succeeds are `CommitOutcomeUnknown`, with reopen selecting exactly the predecessor or complete candidate. Bootstrap rejection intentionally conflates a wrong key with encrypted-header corruption, while defects beneath an authenticated publication are `Corrupt`. V1 provides no repair, salvage, or degraded reads. These boundaries are recorded in ADRs [0042](../../../docs/adr/0042-never-fall-back-beneath-an-authenticated-publication.md) through [0045](../../../docs/adr/0045-keep-btree-open-byte-preserving.md).
