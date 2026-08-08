# 03 — Freeze recovery, maintenance, and verification behavior

**What to build:** Complete the executable behavioral contract for every durable state and operator workflow so implementation agents can build recovery and maintenance without making format-level policy decisions.

**Blocked by:** 02 — Freeze the portable B+ tree and Blob formats.

**Status:** ready-for-agent

- [ ] Every write, publication, and stable-storage-barrier interruption point maps to a precise reopen outcome and committed-generation boundary.
- [ ] Incomplete commit, corruption, authentication failure, wrong key, unsupported format, and provider failure remain distinguishable where the format permits it.
- [ ] Repair, salvage, read-only continuation, and fail-closed boundaries are explicit; no behavior is left to implementation discretion.
- [ ] Checkpoint, reclamation or compaction, online backup, integrity verification, clean close, interruption, and insufficient-space semantics are complete.
- [ ] Backend conformance, reference-model, fault-injection, corruption, fuzzing, portability, provider, concurrency, and performance gates have observable pass criteria.
