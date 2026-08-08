# 09 — Support concurrent snapshots and serialized writers

**What to build:** Allow many threads to hold stable read snapshots while one serialized writer commits, with observable pressure when old storage cannot yet be reclaimed.

**Blocked by:** 06 — Durably transact exact key operations.

**Status:** ready-for-agent

- [ ] Read transactions retain one committed snapshot for their lifetime and do not change after concurrent commits.
- [ ] Write transactions serialize according to the specified contention policy and never expose uncommitted mutations to readers.
- [ ] Reclamation respects every live snapshot and makes newly unreachable storage reusable after the last retaining reader ends.
- [ ] Diagnostics report oldest retained generation, retained or reclaimable space, active-reader pressure, and relevant capacity failures without expiring readers.
- [ ] Concurrency stress and supported-platform sanitizer runs cover readers, writers, rollback, commit failure, and shutdown races.
