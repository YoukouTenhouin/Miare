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
- Public operations use `Result<T, E>` only for legitimate semantic alternatives; invalid contracts and failures to fulfill an operation throw stable `ContractError` or `DatabaseError` categories.
- Canonical terminology lives in [CONTEXT.md](../../CONTEXT.md); settled architectural constraints are recorded in [the ADRs](../../docs/adr/).

## Decisions so far

<!-- Resolved tickets are indexed here; the detailed answer lives in each ticket. -->

- [Freeze the public and transactional contract](../embedded-kv-v1-implementation/issues/01-freeze-public-and-transactional-contract.md) — Fixes the C++20 surface, semantic outcomes and exceptions, handle lifecycles, concurrency and snapshots, Blob streams, maintenance, diagnostics, shutdown, capacity profiles, and representative desktop workload.

- [Freeze the portable B+ tree and Blob formats](../embedded-kv-v1-implementation/issues/02-freeze-portable-btree-and-blob-formats.md) — Fixes copy-on-write generations, the common region and publication slots, authenticated extents, page and allocator formats, Blob manifests and chunks, compression, key derivation, and compatibility behavior.

- [Freeze recovery, maintenance, and verification behavior](../embedded-kv-v1-implementation/issues/03-freeze-recovery-maintenance-and-verification.md) — Fixes interruption and recovery outcomes, fail-closed corruption boundaries, checkpoint, compaction, backup, verification, clean close, storage-exhaustion behavior, and measurable release gates.

- [Use XChaCha20-Poly1305-IETF](../../docs/adr/0015-use-xchacha20-poly1305-ietf.md) — Replaces the earlier agent-assumed AES suite with the explicit product choice of libsodium-compatible XChaCha20-Poly1305-IETF using fresh random 24-byte nonces and full tags.

- [Derive database keys with BLAKE2b](../../docs/adr/0018-derive-database-keys-with-blake2b.md) — Replaces the inherited HKDF-SHA-256 assumption with a salted database-root derivation and libsodium BLAKE2b KDF domain keys.

- [Evaluate compression provider options](issues/08-evaluate-compression-provider-options.md) — Select RFC 8878 Zstandard with `libzstd` as v1's sole bounded, streamable provider; retain LZ4 Frame as a benchmark-driven future option behind database-owned codec/profile IDs.

- [Establish cross-platform durability primitives](issues/03-establish-cross-platform-durability-primitives.md) — Durable commits use positioned I/O and two explicit stable-storage barriers around a self-validating publication record; namespace operations and optional I/O features are not commit primitives.

## Deferred implementation detail

- Distribution and provider-discovery details across build systems remain implementation work within the frozen provider and format contracts.
- Repair, salvage, and degraded reads are deliberately excluded from v1 rather than unspecified; a future separately named offline tool requires its own public contract.

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
