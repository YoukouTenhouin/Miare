# 01 — Freeze the public and transactional contract

**What to build:** Turn the agreed product boundary into the final implementable public contract: applications can understand exactly how database, transaction, cursor, and Blob handles behave; which semantic alternatives are returned and which failures throw; and which workload limits v1 supports.

**Blocked by:** None — can start immediately.

**Status:** resolved

- [x] The contract fixes the C++20 API shapes for creation, opening, transactions, exact operations, scans, Blob streaming, diagnostics, maintenance, and shutdown without requiring a separately built project library.
- [x] Stable error categories and post-failure handle states are defined for every public operation, including contention, commit failure, invalid configuration, resource exhaustion, and misuse.
- [x] Ownership, lifetime, thread-affinity, cursor-mutation, snapshot, read-your-writes, commit, rollback, and shutdown rules are unambiguous and mutually consistent.
- [x] Key, value, Blob, transaction, reader, database-size, cache, and file-growth limits are explicit and tied to representative desktop workloads.
- [x] The contract uses the project glossary and preserves all settled ADR constraints and v1 exclusions.

## Resolution

The frozen contract is [Public and transactional contract](../../../docs/public-transaction-contract.md). It defines the complete public type and method surface, semantic `Result<T, E>` outcomes, stable exception categories, strong pre-persistence failure guarantees, recovery-required transitions, exclusive ownership and thread affinity, transaction and snapshot visibility, cursor and Blob lifecycles, synchronous maintenance and verification, diagnostics and shutdown, compile-time capacity profiles, runtime budgets, and the representative desktop qualification workload.

The contract also records the explicit product correction that v1 uses libsodium-compatible XChaCha20-Poly1305-IETF rather than the earlier agent-assumed AES-256-GCM-SIV suite, and applies the agreed public naming convention.
