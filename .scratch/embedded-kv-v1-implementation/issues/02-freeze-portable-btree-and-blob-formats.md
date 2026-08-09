# 02 — Freeze the portable B+ tree and Blob formats

**What to build:** Define the exact cross-platform representation that lets independent implementations create and read the same portable database file, including the B+ tree, Blobs, authenticated encryption, transparent compression, and format compatibility behavior.

**Blocked by:** 01 — Freeze the public and transactional contract.

**Status:** resolved

- [x] The common bootstrap and backend-owned regions have canonical byte representations with explicit version, backend, feature, identity, root, generation, and integrity metadata.
- [x] Page, extent, free-space, overflow-value, B+ tree update, allocation, and reclamation representations are fully specified.
- [x] Blob identifiers, manifests, chunks, streaming access, deletion, orphan handling, and reclamation have exact persistent representations and lifecycle rules.
- [x] Authenticated units define canonical associated data, domain-separated keys, nonces, tags, usage limits, and fail-closed parsing without releasing unauthenticated plaintext.
- [x] Zstandard units, database-owned codec/profile identifiers, decode bounds, minimum-savings behavior, Blob chunking, and checksum policy are fixed without persisting provider versions.
- [x] Open, backward-readability, upgrade, unsupported-version, unsupported-feature, and incompatible-backend outcomes are explicit.

## Resolution

The frozen representation is [Portable B+ tree and Blob format](../../../docs/portable-btree-blob-format.md). It specifies the immutable copy-on-write generation model, fixed common region and dual publication slots, self-framing authenticated extents, shared slotted-page indexes, overflow Values, Blob catalogs and manifests, configurable chunks, free and retired extent indexes, transaction-local staging, allocator preflight, byte-exact key derivation and associated data, bounded Zstandard profile, and explicit compatibility and fail-closed open behavior.

The v1 B+ tree backend keeps all data-bearing runtime state in the main file. Ticket 03 may deepen interruption, maintenance, recovery, and verification workflows, but may not change these frozen bytes or invariants without an explicit compatibility-breaking decision.
