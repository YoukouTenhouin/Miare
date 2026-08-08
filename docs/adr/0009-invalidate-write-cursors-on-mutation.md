# Keep cursors navigational and invalidate them on write mutation

Cursors are navigation-only and expose no mutation operations. A cursor created from a write transaction initially sees that transaction's uncommitted changes, but a later keyspace mutation invalidates every cursor belonging to that transaction and subsequent use throws `ContractError`; callers recreate cursors to observe the new view. This deliberately rejects backend-specific cursor-repair behavior while read-transaction cursors remain stable for their snapshot's lifetime.
