# 12 — Complete Blob lifecycle and reclamation

**What to build:** Make Blob replacement, deletion, snapshot visibility, orphan handling, and storage reuse transactionally safe under failure and concurrent readers.

**Blocked by:** 11 — Stream transactional Blobs.

**Status:** ready-for-agent

- [ ] Replacement and deletion follow the frozen Blob identifier semantics and atomically compose with ordinary key mutations.
- [ ] A live read snapshot can finish reading an older Blob version after a writer replaces or deletes it.
- [ ] Abandoned creation, rollback, failed commit, and removed application references follow the specified orphan policy without leaking permanently unreachable allocations.
- [ ] Blob chunks become reusable only after no live snapshot can observe them, and diagnostics include Blob-related retained and reclaimable space.
- [ ] Randomized key-and-Blob histories verify content, visibility, atomicity, reclamation, and reopen behavior against an independent model.
