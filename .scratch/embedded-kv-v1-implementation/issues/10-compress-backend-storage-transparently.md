# 10 — Compress backend-owned storage transparently

**What to build:** Honor the immutable creation-time compression policy by storing eligible B+ tree units as bounded Zstandard frames while keeping reads and transactions unaware of the physical representation.

**Blocked by:** 07 — Grow the ordered keyspace beyond a single page.

**Status:** ready-for-agent

- [ ] Databases created with compression disabled never require the compression provider for their B+ tree data, while enabled databases use the frozen profile and savings rules.
- [ ] Compressed and uncompressed units coexist as permitted by the format and yield identical public exact and scan results.
- [ ] Decode output, window memory, frame size, and provider resource use are bounded before allocation or plaintext release.
- [ ] Codec identity, profile, lengths, and compression facts are authenticated so substitution or metadata tampering fails closed.
- [ ] Provider failures, malformed frames, incompatible profiles, and unavailable support surface through stable public errors without changing committed state.
