# Specify the portable file format and compatibility policy

Type: grilling
Status: resolved
Blocked by: 04

## Question

How must the portable database file identify the format version, backend, features, byte order, integrity boundaries, roots and recovery state; which portions are common versus backend-owned; and what open, upgrade, backward-readability, and unsupported-version behavior should v1 promise?

## Resolution

Resolved by the frozen [portable B+ tree and Blob format](../../../docs/portable-btree-blob-format.md), including the exact common region, bootstrap, publication slots, primitive encodings, identifiers, roots, authenticated extents, and compatibility/error policy.
