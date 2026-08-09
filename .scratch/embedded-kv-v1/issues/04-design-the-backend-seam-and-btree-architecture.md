# Design the backend seam and B+ tree architecture

Type: grilling
Status: resolved
Blocked by: 02, 03

## Question

Which responsibilities belong to the backend-independent transactional core versus a storage backend, and which B+ tree update, allocation, reclamation, and recovery architecture should v1 specify so that it satisfies the transaction contract without making a future LSM-tree backend unnatural?

## Resolution

Resolved by the frozen [portable B+ tree and Blob format](../../../docs/portable-btree-blob-format.md): the v1 backend publishes immutable copy-on-write root sets, uses direct self-framing extents and shared slotted-page indexes, and persists free and generation-retired allocation runs behind a backend-owned root.
