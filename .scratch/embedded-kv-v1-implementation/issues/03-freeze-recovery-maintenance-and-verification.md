# 03 — Freeze recovery, maintenance, and verification behavior

**What to build:** Complete the executable behavioral contract for every durable state and operator workflow so implementation agents can build recovery and maintenance without making format-level policy decisions.

**Blocked by:** 02 — Freeze the portable B+ tree and Blob formats.

**Status:** resolved

- [x] Every write, publication, and stable-storage-barrier interruption point maps to a precise reopen outcome and committed-generation boundary.
- [x] Incomplete commit, corruption, authentication failure, wrong key, unsupported format, and provider failure remain distinguishable where the format permits it.
- [x] Repair, salvage, read-only continuation, and fail-closed boundaries are explicit; no behavior is left to implementation discretion.
- [x] Checkpoint, reclamation or compaction, online backup, integrity verification, clean close, interruption, and insufficient-space semantics are complete.
- [x] Backend conformance, reference-model, fault-injection, corruption, fuzzing, portability, provider, concurrency, and performance gates have observable pass criteria.

## Resolution

The frozen behavior is [Recovery, maintenance, and verification contract](../../../docs/recovery-maintenance-verification-contract.md). It defines byte-preserving authenticated crash recovery, the exact transaction and maintenance publication boundaries, fail-closed corruption behavior without v1 repair or salvage, reclamation checkpointing, online in-place compaction, verified physical-prefix backup, authoritative integrity verification, cause-dependent close, storage-exhaustion outcomes, stable reports, and observable release gates.

The v1 B+ tree backend remains sidecar-free and format-compatible with ticket 02. This contract deepens operator workflows and tests without changing the common region, publication slots, extent framing, B+ tree, Blob, allocator, encryption, or compression bytes already frozen there.
