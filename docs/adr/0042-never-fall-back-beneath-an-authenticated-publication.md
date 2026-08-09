# Never fall back beneath an authenticated publication

Open may select the predecessor only when a newer designated publication slot is incomplete, structurally invalid, or unauthenticated. Once the newest slot authenticates, it establishes the committed generation and corrupt, missing, truncated, or incompatible referenced roots fail open rather than causing selection of older data. This preserves recovery from torn publication while refusing to disguise damage to known committed state as a successful stale rollback.
