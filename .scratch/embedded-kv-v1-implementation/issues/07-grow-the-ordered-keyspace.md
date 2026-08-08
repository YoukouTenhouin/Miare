# 07 — Grow the ordered keyspace beyond a single page

**What to build:** Preserve durable exact-operation behavior as the ordered keyspace grows through B+ tree levels, page splits, overflow values, reuse, and substantial mixed workloads.

**Blocked by:** 06 — Durably transact exact key operations.

**Status:** ready-for-agent

- [ ] Inserts create and split leaf and internal pages through multiple tree levels while every committed key remains exactly retrievable.
- [ ] Ordinary values up to the documented bound use the specified inline or overflow representation and remain atomic across overwrite and deletion.
- [ ] Allocation and safe reclamation reuse storage without exposing unreachable, stale, or uncommitted content.
- [ ] Authenticated page and overflow boundaries detect truncation, substitution, reordering, and malformed lengths before returning data.
- [ ] Randomized exact-operation histories match an independent ordered-keyspace reference model across commit, rollback, close, and reopen.
