# 06 — Durably transact exact key operations

**What to build:** Let applications atomically read, insert, overwrite, and delete ordinary values in one write transaction, with explicit commit or rollback and durable results after reopening.

**Blocked by:** 03 — Freeze recovery, maintenance, and verification behavior; 05 — Create and reopen an empty encrypted database.

**Status:** ready-for-agent

- [ ] Read and write transactions support exact lookup over arbitrary byte-string keys using unsigned-byte lexicographic identity.
- [ ] A write transaction provides read-your-writes for insertion, overwrite, and deletion and atomically commits multiple exact mutations.
- [ ] Explicit rollback, handle destruction before commit, validation failure, and injected commit failure leave the previously committed state visible.
- [ ] Successful commit follows the specified write, barrier, publication-record, barrier sequence and survives close and reopen.
- [ ] Deterministic interruption tests around the first exact commits expose either the old or new committed state, never a partial mixture.
