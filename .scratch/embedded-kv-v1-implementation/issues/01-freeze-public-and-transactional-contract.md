# 01 — Freeze the public and transactional contract

**What to build:** Turn the agreed product boundary into the final implementable public contract: applications can understand exactly how database, transaction, cursor, and Blob handles behave; which routine failures are returned; and which workload limits v1 supports.

**Blocked by:** None — can start immediately.

**Status:** ready-for-agent

- [ ] The contract fixes the C++20 API shapes for creation, opening, transactions, exact operations, scans, Blob streaming, diagnostics, maintenance, and shutdown without requiring a separately built project library.
- [ ] Stable error categories and post-failure handle states are defined for every public operation, including contention, commit failure, invalid configuration, resource exhaustion, and misuse.
- [ ] Ownership, lifetime, thread-affinity, cursor-mutation, snapshot, read-your-writes, commit, rollback, and shutdown rules are unambiguous and mutually consistent.
- [ ] Key, value, Blob, transaction, reader, database-size, cache, and file-growth limits are explicit and tied to representative desktop workloads.
- [ ] The contract uses the project glossary and preserves all settled ADR constraints and v1 exclusions.
