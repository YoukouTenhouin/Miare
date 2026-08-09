# 06 — Durably transact exact key operations

**What to build:** Let applications atomically read, insert, overwrite, and delete ordinary values in one write transaction, with explicit commit or rollback and durable results after reopening.

**Blocked by:** 03 — Freeze recovery, maintenance, and verification behavior; 05 — Create and reopen an empty encrypted database.

**Status:** resolved

- [x] Read and write transactions support exact lookup over arbitrary byte-string keys using unsigned-byte lexicographic identity.
- [x] A write transaction provides read-your-writes for insertion, overwrite, and deletion and atomically commits multiple exact mutations.
- [x] Explicit rollback, handle destruction before commit, validation failure, and injected commit failure leave the previously committed state visible.
- [x] Successful commit follows the specified write, barrier, publication-record, barrier sequence and survives close and reopen.
- [x] Deterministic interruption tests around the first exact commits expose either the old or new committed state, never a partial mixture.

## Resolution

`Database` now creates allocator-aware read results and move-only read and write
transaction handles with stable snapshots, one-writer admission, thread affinity,
read-your-writes, exact `get` and `contains`, unconditional `put`, conditional
`erase`, explicit terminal operations, and mutation statistics. Exact keys use
unsigned-byte lexicographic identity, including empty and high-bit keys. The
single-leaf foundation persists canonical prefix-compressed ordered leaf images
in authenticated, optionally compressed extents; page growth, splitting, and
overflow Values remain the work of Ticket 07.

Commit preparation completes before persistence. A mutating commit writes its
new immutable extent, crosses a stable-storage barrier, writes the designated
encrypted publication slot, and crosses a second barrier. Pre-publication
failures retain the active transaction; persistence-stage failures terminate it,
enter recovery-required state, and distinguish a known unpublished result from
an outcome requiring reopen selection. Successful publications alternate slots,
increment generations, and survive close and reopen.

The exact-transaction suite covers atomic multi-key insertion, overwrite and
deletion, empty and unsigned-byte keys, read snapshots, writer admission, handle
movement and affinity, rollback and destruction, validation and provider
preflight failures, both commit-failure classifications, operation ordering,
successive durable generations, and deterministic crash images at the data and
publication boundaries. Every interruption image selects either the predecessor
or the complete successor generation.
