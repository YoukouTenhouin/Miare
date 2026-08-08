# 02 — Freeze the portable B+ tree and Blob formats

**What to build:** Define the exact cross-platform representation that lets independent implementations create and read the same portable database file, including the B+ tree, Blobs, authenticated encryption, transparent compression, and format compatibility behavior.

**Blocked by:** 01 — Freeze the public and transactional contract.

**Status:** ready-for-agent

- [ ] The common bootstrap and backend-owned regions have canonical byte representations with explicit version, backend, feature, identity, root, generation, and integrity metadata.
- [ ] Page, extent, free-space, overflow-value, B+ tree update, allocation, and reclamation representations are fully specified.
- [ ] Blob identifiers, manifests, chunks, streaming access, deletion, orphan handling, and reclamation have exact persistent representations and lifecycle rules.
- [ ] Authenticated units define canonical associated data, domain-separated keys, nonces, tags, usage limits, and fail-closed parsing without releasing unauthenticated plaintext.
- [ ] Zstandard units, database-owned codec/profile identifiers, decode bounds, minimum-savings behavior, Blob chunking, and checksum policy are fixed without persisting provider versions.
- [ ] Open, backward-readability, upgrade, unsupported-version, unsupported-feature, and incompatible-backend outcomes are explicit.
