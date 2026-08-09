# Design the compression policy and provider contract

Type: grilling
Status: resolved
Blocked by: 04, 08

## Question

What must the database-creation compression option mean; how should the B+ tree decide whether and where to compress ordinary values and Blob chunks; how are codec identity, thresholds, bounds, errors, and future format evolution represented; and what provider interface preserves transparent reads without user-defined codecs?

## Resolution

Resolved by Zstandard profile 1 in the frozen [portable B+ tree and Blob format](../../../docs/portable-btree-blob-format.md): whole authenticated units, level 3, bounded 16 MiB windows, explicit sizes, no dictionary or checksum, and retention only when at least one Allocation quantum is saved.
