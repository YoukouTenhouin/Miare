# 08 — Traverse ordered, prefix, and bounded ranges

**What to build:** Let applications traverse the ordered keyspace predictably with cursors, prefix scans, and bounded ranges while retaining the transaction's stable view.

**Blocked by:** 07 — Grow the ordered keyspace beyond a single page.

**Status:** ready-for-agent

- [ ] Forward and reverse cursor traversal observes unsigned-byte lexicographic ordering across leaf boundaries.
- [ ] Prefix scans return exactly the matching interval for arbitrary byte prefixes, including empty and all-`0xff` boundary cases.
- [ ] Inclusive and exclusive bounded ranges honor empty, open, and unbounded endpoints as defined by the public contract.
- [ ] Cursor positioning, end-state, copying or movement, lifetime, and write-transaction mutation behavior match the frozen rules.
- [ ] Model-based histories cover traversal during inserts, overwrites, deletions, rollback, commit, and reopen without asserting private page structure.
