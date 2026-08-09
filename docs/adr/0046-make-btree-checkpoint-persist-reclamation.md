# Make B+ tree checkpoint persist safe reclamation

For the sidecar-free B+ tree backend, `checkpoint()` durably moves snapshot-safe retired runs into the canonical free index and stabilizes removal of any abandoned tail, publishing a generation only when allocation state changes. It neither relocates reachable extents nor lowers the committed high-water mark and performs no full content verification; this preserves a useful backend-independent checkpoint operation without conflating it with compaction or verification, and a call with nothing to persist is a barrier-free no-op.
