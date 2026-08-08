# Chart an implementation-ready embedded KV database v1 specification

Label: wayfinder:map

## Destination

Produce an implementation-ready v1 product and architecture specification for a portable, transactional, encrypted and compressed embedded KV database. The completed specification must settle the public contract, storage architecture, file and recovery formats, provider boundaries, safety guarantees, and verification criteria without implementing the database.

## Notes

- Use the `grilling` and `domain-modeling` skills for product decisions, `codebase-design` for module seams, `prototype` for API sketches, and `research` for claims that depend on external standards or platform behavior.
- The library's own implementation is header-only C++20 and targets Windows, Linux, and macOS. Vetted cryptography and compression providers may be vendored or separately linked.
- V1 serves many threads in one process, with concurrent snapshot readers and one serialized writer. Multi-process access is outside the project boundary.
- Transactions are ACID, atomically cover multiple keys and blobs, and provide durable commits only. Nested transactions are excluded.
- There is one ordered byte-string keyspace using unsigned-byte lexicographic order. Exact lookup, ordered cursors, prefix scans, and bounded range scans are required; custom comparators and named collections are excluded.
- Ordinary values are arbitrary in-memory byte strings. Large assets use a distinct transactional Blob facility with incremental I/O and remain inside the portable database file.
- A storage backend is selected permanently at database creation. V1 ships a B+ tree backend while preserving a seam for later LSM-tree and other backends; existing databases cannot migrate between backends.
- Compression is transparent, enabled at database creation, and physically implemented by each backend. User-defined codecs are excluded from v1.
- Encryption protects confidentiality and integrity against offline file access, not a compromised running process. The caller supplies high-entropy key material; passwords, KDF policy, and OS key storage remain application concerns.
- A cleanly closed database's committed state is portable as one cross-platform file. Runtime lock, journal, or WAL sidecars are permitted, but data-bearing sidecars must be encrypted and must not be needed after a successful close.
- Read transactions retain stable snapshots and do not block the writer, but may delay reclamation; diagnostics should expose that pressure rather than forcibly expiring readers.
- Public operations return explicit result/error values for routine failures rather than throwing exceptions.
- Canonical terminology lives in [CONTEXT.md](../../CONTEXT.md); settled architectural constraints are recorded in [the ADRs](../../docs/adr/).

## Decisions so far

<!-- Resolved tickets are indexed here; the detailed answer lives in each ticket. -->

- [Establish authenticated-encryption constraints](issues/06-establish-authenticated-encryption-constraints.md) — Fixes v1 on AES-256-GCM-SIV with HKDF-separated domains, fresh random nonces, full 128-bit tags, independently authenticated bounded units, canonical AAD, and explicit use/rollback limits.

- [Evaluate compression provider options](issues/08-evaluate-compression-provider-options.md) — Select RFC 8878 Zstandard with `libzstd` as v1's sole bounded, streamable provider; retain LZ4 Frame as a benchmark-driven future option behind database-owned codec/profile IDs.

- [Establish cross-platform durability primitives](issues/03-establish-cross-platform-durability-primitives.md) — Durable commits use positioned I/O and two explicit stable-storage barriers around a self-validating publication record; namespace operations and optional I/O features are not commit primitives.

## Not yet specified

- Exact page, extent, free-space, and overflow layouts; these become sharp after the B+ tree update/recovery strategy and Blob model are chosen.
- Format upgrade and backward-compatibility mechanics beyond the already-required cross-platform byte representation; these depend on the physical format and encryption envelope.
- Concrete salvage and repair behavior for partially corrupt databases; this depends on what integrity metadata and failure boundaries the selected formats provide.
- Exact cache controls, memory-pressure behavior, database-size limits, key/value limits, and performance targets; these depend on the API and storage architecture.
- Detailed fuzzing, fault-injection, crash-matrix, interoperability, and long-duration tests; these become precise after the observable contracts and file states are settled.
- Distribution and provider-discovery details across build systems; these depend on the final provider interfaces and supported configurations.

## Out of scope

- Implementing the library, sequencing implementation work, or planning a release; those begin only after this map is set in stone.
- Simultaneous access from multiple processes.
- Implementing an LSM-tree or any second production backend in v1.
- Migrating an existing database between storage backends.
- Custom key comparators, typed serialization, secondary indexes, and multiple named KV collections.
- Password handling, password KDF policy, OS keychain integration, and protection from a compromised running process.
- User-pluggable compression codecs and a zero-link-dependency guarantee.
- Weaker or configurable commit durability modes.
- Streaming multi-gigabyte payloads through the ordinary value API; the Blob facility owns that use case.
- Replication, synchronization, remote access, and server operation.
