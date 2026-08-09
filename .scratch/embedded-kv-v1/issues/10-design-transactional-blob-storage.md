# Design transactional Blob semantics and storage

Type: grilling
Status: resolved
Blocked by: 01, 02, 04, 07, 09

## Question

What lifecycle, identifier, streaming, visibility, atomicity, overwrite, deletion, orphan handling, size, random-access, snapshot, compression, encryption, and failure semantics should Blobs expose, and how should the B+ tree backend store and reclaim Blob chunks inside the portable database file?

## Resolution

Resolved by the frozen [public contract](../../../docs/public-transaction-contract.md) and [portable B+ tree and Blob format](../../../docs/portable-btree-blob-format.md): stable catalog identity, immutable per-version manifests, configurable independently protected chunks, dense chunk indexes, seekable snapshots, replacement/deletion, and reachability-based orphan reclamation.
