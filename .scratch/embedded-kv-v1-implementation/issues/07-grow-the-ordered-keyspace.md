# 07 — Grow the ordered keyspace beyond a single page

**What to build:** Preserve durable exact-operation behavior as the ordered keyspace grows through B+ tree levels, page splits, overflow values, reuse, and substantial mixed workloads.

**Blocked by:** 06 — Durably transact exact key operations.

**Status:** resolved

- [x] Inserts create and split leaf and internal pages through multiple tree levels while every committed key remains exactly retrievable.
- [x] Ordinary values up to the documented bound use the specified inline or overflow representation and remain atomic across overwrite and deletion.
- [x] Allocation and safe reclamation reuse storage without exposing unreachable, stale, or uncommitted content.
- [x] Authenticated page and overflow boundaries detect truncation, substitution, reordering, and malformed lengths before returning data.
- [x] Randomized exact-operation histories match an independent ordered-keyspace reference model across commit, rollback, close, and reopen.

## Resolution

The ordered keyspace now grows as a persistent authenticated B+ tree with
canonical leaf and internal pages, recursive balanced splitting, complete
separator routing, copy-on-write updates, lazy deletion, empty-page removal,
and root collapse. Clean subtrees retain their authenticated Extent references,
while affected paths are rewritten locally. A transaction that empties a
multi-level tree and inserts a replacement keyset rebuilds a canonical leaf
root instead of retaining an empty internal topology.

Values at or below the capacity profile's inline cutoff remain in leaf entries;
larger Values use independently authenticated overflow extents with exact
decoded-length bounds. Loading validates physical framing, roles, levels,
prefix-compressed slots, subtree ordering, separators, overflow lengths, and
authentication before returning content. Open performs the frozen shallow-root
admission, while deeper corruption discovered on first access atomically enters
recovery-required state.

Every committed generation publishes an authenticated allocator root whose
free and generation-retired run indexes use the shared multi-level page format.
Lowest-address first-fit allocation reuses safe runs, live process snapshots
delay reclamation by retirement generation, and retained copy-on-write extents
are removed from retirement state with an interval sweep. Conservative
full-page metadata reservation converges before publication, including when
the final authenticated pages compress to smaller physical spans.

The ordered-growth suite covers multi-level growth and reopen, overflow
replacement and deletion, copy-on-write path retention, root collapse and
replacement, fragmented allocator indexes, snapshot-delayed reuse, malformed
authenticated structures, rollback and commit histories, and shallow-open
behavior. Its qualification mode runs 1,000 deterministic seeds with 10,000
operations each against an independent ordered map across commit, rollback,
close, and reopen. Release and sanitizer runs pass locally, and the PR passes
the Linux, macOS, and Windows CI matrix.
