# 11 — Stream transactional Blobs

**What to build:** Let applications create Blob identifiers and incrementally write and read large Blob content inside transactions without loading the entire Blob into memory.

**Blocked by:** 08 — Traverse ordered, prefix, and bounded ranges; 09 — Support concurrent snapshots and serialized writers; 10 — Compress backend-owned storage transparently.

**Status:** ready-for-agent

- [ ] A write transaction can create a stable database-local Blob identifier and stream content in bounded chunks up to the documented limit.
- [ ] The same write transaction can store the Blob identifier in a value and read its uncommitted Blob content according to read-your-writes rules.
- [ ] Commit publishes the value and complete Blob atomically; rollback, incomplete streaming, or commit failure publishes neither.
- [ ] Read transactions incrementally access Blob content from their stable snapshot with the specified sequential and random-access behavior.
- [ ] Each Blob chunk is independently compressed when beneficial, authenticated in order, resource-bounded, and portable across supported operating systems.
