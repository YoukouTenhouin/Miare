# Embedded transactional key-value database v1

Status: ready-for-agent

## Problem Statement

Desktop application developers need an embedded database that can be linked directly into a C++20 application while providing durable transactions, concurrent snapshot reads, ordered byte-string access, transactional storage for large Blobs, transparent compression, authenticated encryption, and a portable database file across Windows, Linux, and macOS. The desired product boundary is understood, but implementation cannot begin safely until the public contract, storage and provider seams, physical formats, failure behavior, capacity envelope, and verification gates form one coherent v1 specification.

## Solution

Specify a header-only C++20 embedded database with one ordered keyspace of arbitrary byte-string keys and values, exact lookup and ordered scans, ACID transactions, concurrent snapshot readers and one serialized writer, and first-class transactional Blobs for incrementally accessed large data. A storage backend is selected permanently at database creation; v1 supplies a B+ tree backend behind a common transactional contract. The selected backend owns its physical compression units and uses transparent Zstandard compression when the creation-time compression policy permits it.

Protect persistent content with independently authenticated libsodium-compatible XChaCha20-Poly1305-IETF units, fresh random 24-byte nonces, full 16-byte tags, and purpose-separated derived keys. The v1 B+ tree backend keeps all data-bearing runtime state in its main file and publishes immutable generations through dual authenticated slots. Scope the durability promise to validated local regular files whose storage stack honors the required barriers. Complete the remaining specification by fixing interruption and recovery behavior, maintenance algorithms, and observable conformance gates before implementing those layers.

## User Stories

1. As a desktop application developer, I want to include the database project's C++20 implementation without building a separate database library, so that integration remains simple across supported toolchains.
2. As a desktop application developer, I want vetted cryptography and compression providers to remain outside the header-only promise, so that security-sensitive primitives are maintained by appropriate upstream implementations.
3. As an application developer, I want to create or open a database through a small explicit API, so that ownership, configuration, and failure behavior are clear.
4. As an application developer, I want legitimate alternative outcomes returned as explicit `Result<T, E>` values and failures to fulfill an operation reported by stable exceptions, so that control flow follows operation semantics.
5. As an application developer, I want arbitrary byte-string keys and values, so that serialization remains an application concern.
6. As an application developer, I want keys ordered lexicographically by unsigned byte value, so that ordering is stable and portable.
7. As an application developer, I want exact key lookup, so that I can retrieve an individual value efficiently.
8. As an application developer, I want ordered cursors, so that I can traverse the ordered keyspace predictably.
9. As an application developer, I want prefix scans, so that related keys can be enumerated without named collections or secondary indexes.
10. As an application developer, I want bounded range scans, so that I can process a selected interval of the ordered keyspace.
11. As an application developer, I want transactionally consistent reads, so that related keys and Blobs are observed from one committed state.
12. As a reader, I want a stable snapshot for the life of my read transaction, so that concurrent commits do not change results midway through an operation.
13. As an application developer, I want many threads to hold read transactions concurrently, so that read-heavy desktop workloads can scale within one process.
14. As a writer, I want one serialized write transaction with read-your-writes behavior, so that multi-step updates are predictable.
15. As an application developer, I want commits to be durable rather than offering weaker durability modes, so that a successful commit has one unambiguous meaning.
16. As an application developer, I want updates to multiple keys and Blobs to commit or roll back atomically, so that references and their Blob data cannot diverge.
17. As an application developer, I want deletion, overwrite, rollback, commit failure, writer contention, and shutdown behavior specified precisely, so that edge cases do not depend on implementation accident.
18. As an application developer, I want transaction, cursor, Blob, and database handle lifetime and thread-affinity rules, so that misuse is difficult and diagnosable.
19. As an application developer, I want long-lived readers to delay reclamation without being forcibly expired, so that snapshot correctness is preserved.
20. As an operator, I want diagnostics to expose reclamation and resource pressure caused by long-lived readers, so that application behavior can be corrected before capacity is exhausted.
21. As an application developer, I want ordinary values to have a documented in-memory size bound, so that resource use is predictable.
22. As an application developer, I want potentially large byte sequences stored as first-class Blobs, so that large assets do not have to pass through the ordinary value API.
23. As an application developer, I want incremental Blob I/O within a transaction, so that large assets do not have to fit in memory.
24. As an application developer, I want stable database-local Blob identifiers that values can store, so that application data can refer to Blobs without external paths or URLs.
25. As an application developer, I want Blob creation, visibility, overwrite, deletion, orphan handling, and reclamation semantics specified, so that Blob lifecycles remain transactionally safe.
26. As an application developer, I want Blob data stored inside the portable database file, so that backup, movement, encryption, and compression use the same database boundary.
27. As an application developer, I want to select a storage backend only when creating a database, so that the on-disk format cannot change unexpectedly.
28. As an application developer, I want v1 to provide a production B+ tree storage backend, so that the first release has one complete persistence strategy.
29. As a future backend author, I want a common transactional contract that does not encode B+ tree assumptions, so that a later LSM-tree or other storage backend remains possible.
30. As an application developer, I want a database to reject an attempt to open it with an incompatible storage backend, so that unsupported cross-format migration cannot corrupt data.
31. As an application developer, I want transparent compression enabled by a creation-time compression policy, so that storage savings do not complicate reads and writes.
32. As a storage-backend author, I want the storage backend to choose its physical compression unit, so that a B+ tree and a future LSM-tree can use compression naturally.
33. As an application developer, I want Zstandard to be the single v1 compression format with explicit decode-memory and output bounds, so that compressed input cannot cause unbounded resource use.
34. As an application developer, I want database-owned codec and profile identifiers rather than persisted provider versions, so that compatible provider updates do not alter the file contract.
35. As an application developer, I want independently framed Blob chunks, so that large compressed Blobs remain incrementally accessible and corruption has a bounded impact.
36. As an application developer, I want caller-supplied high-entropy key material to protect database content at rest, so that offline possession of database files does not reveal plaintext.
37. As an application developer, I want password derivation and OS key storage left to my application, so that the database does not impose an unsuitable credential policy.
38. As an application developer, I want libsodium-compatible XChaCha20-Poly1305-IETF with random 24-byte nonces and full authentication tags as the only v1 encrypted-format suite, so that confidentiality and integrity guarantees are uniform without depending on AES acceleration.
39. As an application developer, I want independently authenticated bounded pages, records, log frames, and Blob chunks, so that random access and localized validation remain possible.
40. As an application developer, I want authentication to bind database identity, domain, logical identity or location, generation, lengths, compression facts, and sequence information, so that encrypted units cannot be substituted or reordered undetected.
41. As an application developer, I want wrong keys, tampering, malformed ciphertext, and unavailable cryptographic support to fail closed, so that the library never silently downgrades protection or releases unauthenticated plaintext.
42. As a security reviewer, I want cryptographic usage limits, nonce generation, key separation, and sensitive-memory handling specified, so that provider integration preserves the on-disk security contract.
43. As an application developer, I want the offline threat boundary stated explicitly, so that I understand that a compromised running process and whole-file rollback without trusted external state are not prevented.
44. As an application developer, I want a durable commit to use explicit stable-storage barriers around a self-validating publication record, so that recovery can distinguish committed generations after interruption.
45. As an application developer, I want recovery to tolerate short writes, torn writes, permitted reordering, and an interrupted barrier, so that crashes do not expose partially committed state as successful data.
46. As an application developer, I want the durable-commit guarantee scoped to local regular files on truthful filesystem and device stacks, so that the promise does not exceed what supported platforms can provide.
47. As an application developer, I want every data-bearing journal or write-ahead-log sidecar encrypted to the same at-rest standard as the main file, so that runtime recovery files do not leak protected data.
48. As an application developer, I want a successful clean close to consolidate all committed state into the portable database file, so that the database can be copied or moved as one file.
49. As an application developer, I want the portable file representation to be cross-platform, versioned, and self-identifying, so that files can move among supported operating systems and incompatible formats are rejected safely.
50. As an operator, I want open and recovery to distinguish an incomplete commit, post-bootstrap corruption, bootstrap authentication rejection, and unsupported versions while intentionally conflating a wrong key with encrypted-header corruption, so that failures lead to appropriate action without creating an authentication oracle.
51. As an operator, I want explicit checkpoint, compaction or reclamation, consistent backup, integrity verification, and clean-close operations, so that database maintenance is predictable.
52. As an operator, I want interrupted maintenance and insufficient-free-space behavior specified, so that maintenance cannot silently weaken committed data.
53. As an application developer, I want documented limits for keys, values, Blobs, transactions, readers, database size, cache use, and file growth, so that workloads can be validated before deployment.
54. As a product owner, I want representative desktop workload and latency, throughput, memory, and growth targets, so that architecture choices are evaluated against the intended use rather than synthetic convenience.
55. As a backend implementer, I want one backend-conformance suite at the public transactional contract, so that externally visible semantics are verified consistently without coupling tests to internal structures.
56. As a backend implementer, I want a reference model for ordered keyspace and transaction behavior, so that randomized histories can be checked against an independent oracle.
57. As a reliability engineer, I want deterministic fault injection and crash-state enumeration around persistence boundaries, so that every durable file state has a specified recovery outcome.
58. As a security engineer, I want corruption, substitution, reordering, truncation, wrong-key, and provider-failure tests, so that authentication failures are always closed and bounded.
59. As a portability maintainer, I want compatibility fixtures exchanged among Windows, Linux, and macOS, so that the portable database file promise is continuously verified.
60. As a maintainer, I want fuzzing, sanitizer, concurrency stress, long-duration, provider-matrix, and performance gates, so that v1 readiness is based on observable evidence.

## Implementation Decisions

- The project targets C++20 on Windows, Linux, and macOS. The project's own database implementation is header-only; vetted provider dependencies may be vendored or linked separately.
- The public model contains one ordered keyspace. Keys and values are arbitrary byte strings, with keys ordered lexicographically by unsigned byte value. Typed serialization, named collections, custom comparators, and secondary indexes are application concerns or future work.
- The frozen [public and transactional contract](../../docs/public-transaction-contract.md) includes database creation and open, allocator-aware byte ownership, distinct read and write transactions, exact operations, half-open ordered scans, transactional Blob streaming, explicit commit and rollback, synchronous maintenance and verification, diagnostics, and clean shutdown.
- Legitimate semantic alternatives use the expected-like `Result<T, E>` type or `std::optional`; failures to fulfill an operation throw `ContractError` or `DatabaseError` with stable `Errc` categories. The contract fixes affected-handle state for every failure stage.
- V1 is single-process and many-threaded. It permits concurrent snapshot read transactions and one serialized writer. Read transactions do not block the writer but may retain old storage and create observable reclamation pressure.
- Transactions are ACID and durable-only. A write transaction atomically covers multiple keys and Blobs. Nested transactions and configurable weaker durability are excluded.
- Ordinary values are bounded in-memory byte strings. Blobs are a distinct transactional facility with incremental I/O, chunked storage inside the portable database file, and stable database-local identifiers that may be stored in values.
- A storage backend is selected permanently at database creation and recorded in the format. V1 implements a B+ tree backend. Backend migration and a second production backend are excluded, while the backend-independent contract must remain natural for future strategies such as an LSM-tree.
- The backend-independent core owns public semantics and the common bootstrap/publication boundary; the frozen [portable B+ tree and Blob format](../../docs/portable-btree-blob-format.md) owns copy-on-write physical organization, allocation, reclamation, Blob storage, and compression units for backend format 1.
- Compression is transparent and permitted by a database-creation policy. V1 uses RFC 8878 Zstandard through `libzstd` as its sole codec, with independently decodable bounded units, persisted database-owned codec/profile identifiers, strict decode limits, and no user-defined codecs.
- Compression provider versions are not persisted as compatibility identities. Zstandard profile 1 fixes level 3, a 16 MiB decode window, no dictionary or frame checksum, whole-unit compression, and a one-allocation-quantum minimum saving. Blob chunk size is an exact-match capacity-profile value defaulting to 1 MiB. Qualification validates these defaults without changing format 1.
- The encrypted format uses the libsodium-compatible XChaCha20-Poly1305-IETF construction only, with a 32-byte derived key, fresh random 24-byte nonces, and untruncated 16-byte tags. Exact nonce reuse under one derived key is forbidden, and lack of suite support is an error rather than grounds for downgrade.
- Caller-supplied 32-byte high-entropy key material is combined with a random persisted 16-byte salt and canonical database identity using keyed BLAKE2b-256, then the libsodium-compatible BLAKE2b KDF derives separate 32-byte header, main-data, recovery-data, and Blob keys using fixed context and subkey identifiers. Password handling, password KDF policy, and OS key storage remain outside the database.
- Pages or records, recovery-log frames, and Blob chunks are independently authenticated bounded units. Canonical associated data and authenticated roots or manifests bind database identity, domain, unit type, logical identity or location, generation or sequence, lengths, compression facts, and ordering.
- Unauthenticated plaintext is never released. The frozen envelope covers wrong keys, malformed or tampered input, usage limits, nonce generation, provider failure, and post-bootstrap corruption; recovery work retains the fail-stop and rollback boundaries.
- The threat model protects confidentiality and integrity against offline access. It does not protect against a compromised running process and cannot detect rollback of the entire database to a previously valid state without trusted external state.
- Durable file access uses exact positioned reads and writes, resize, and explicit stable-storage barriers: `FlushFileBuffers` on Windows, `fsync` on Linux, and `F_FULLFSYNC` on macOS.
- A durable commit writes recovery or new data, performs a stable-storage barrier, publishes a self-validating generation record, and performs a second barrier. Recovery accounts for short or torn writes, writes reordered before a completed barrier, and interrupted barriers.
- Writable memory mapping is excluded from the canonical v1 write path. Rename, replacement, close, filesystem journaling, sparse allocation, and preallocation are not durable commit primitives. File locking is best-effort defensive detection because multi-process access is outside the project boundary.
- The durable-commit promise applies to an already-open, validated database on local regular-file storage whose filesystem and device stack truthfully honor the required barriers. Namespace operations use the strongest available platform sequence plus reopen validation but do not expand that promise.
- After a successful clean close, the portable database file contains all committed state and is sufficient for transfer. Runtime lock, journal, or write-ahead-log sidecars are allowed, but data-bearing sidecars receive equivalent encryption and are not required after clean close.
- The common and backend-owned regions use the frozen cross-platform byte representation in the portable-format document, including versions, backend and feature identities, exact capacity profile, integrity boundaries, roots, generations, and compatibility outcomes. Open never performs an implicit upgrade.
- B+ tree updates, page and extent bytes, free and retired indexes, overflow Values, Blob physical layout, compression boundaries, and authenticated-open corruption boundaries are frozen. Cache implementation, salvage policy, interruption behavior, and maintenance algorithms remain specification work.
- The completed v1 specification must define checkpointing, compaction or reclamation, online consistent backup, integrity verification, clean close, portable-file recognition, interruption behavior, and required free-space behavior.
- The public contract fixes correctness limits and the representative desktop workload; later qualification adds measurable latency, throughput, memory, and amplification evidence for the already-frozen default format profile.

## Testing Decisions

- The primary and highest test seam is the public database contract, run as a backend-conformance suite against the v1 B+ tree backend and any future backend. Tests assert returned results, visible ordered keyspace and Blob state, transaction isolation, durability, recovery outcomes, diagnostics, and portable files rather than private page structures or call sequences.
- A single conformance harness should cover database lifecycle, exact operations, cursors and scans, read and write transactions, contention, snapshot stability, read-your-writes, deletion and overwrite, Blob atomicity, commit and rollback failures, live-handle shutdown behavior, and resource-pressure diagnostics.
- Model-based and randomized tests should compare public histories against a small independent ordered-keyspace and transaction reference model. This is preferable to asserting B+ tree implementation details.
- Persistence tests need a narrow deterministic fault-injection seam around the durable-file abstraction because crashes, short and torn I/O, reordering, and barrier failure cannot be produced reliably through ordinary public calls. Enumerated interruption points must reopen the resulting file and assert the public recovery outcome.
- Encryption and compression provider adapters need contract tests at their narrow boundaries for bounds, stable format identifiers, provider errors, authentication failure, and streaming behavior. End-to-end tests must still prove that these behaviors surface correctly through database open, reads, writes, recovery, and Blob I/O.
- Compatibility fixtures must be created and consumed across Windows, Linux, and macOS, covering encrypted and compressed configurations, supported format versions, unsupported features, wrong keys, and cleanly closed portability.
- Corruption tests must mutate, truncate, substitute, duplicate, and reorder independently protected units and verify that no unauthenticated plaintext or partially trusted state becomes observable.
- Concurrency stress tests must exercise many readers, writer serialization, long-lived snapshots, reclamation pressure, and shutdown races under sanitizers suitable to each platform.
- Fuzzing should target public operation sequences, file parsing, recovery records, authenticated envelopes, compression frames, cursors, ranges, and Blob chunk manifests using bounded resource settings.
- Performance verification must use the capacity envelope's representative workloads and report latency percentiles, throughput, peak memory, file amplification, reclamation behavior, and clean-close or maintenance costs. It validates the frozen default profile and informs only a future explicit format/profile identity.
- There is no implementation test suite in the repository yet. The closest prior art is the settled durability, authenticated-encryption, and compression research, which defines platform matrices, failure cases, limits, and provider behavior that the conformance and fault-injection suites must turn into executable gates.

## Out of Scope

- Implementing the database as part of this specification effort.
- Multi-process access to the same database.
- An LSM-tree or any second production storage backend in v1.
- Migration of an existing database between storage backends.
- Multiple named key-value collections, custom key comparators, typed serialization, secondary indexes, or query processing.
- Nested transactions or configurable weaker commit-durability modes.
- Treating multi-gigabyte payloads as ordinary values rather than Blobs.
- User-pluggable compression codecs, persistent compression dictionaries without a specified lifecycle, or a zero-link-dependency guarantee.
- Password handling, password-based key derivation policy, OS keychain integration, protection from a compromised running process, or whole-file rollback detection without trusted external state.
- Rekeying, encryption-suite migration, compression migration, or implicit format upgrades; applications create a new database and copy logical content.
- Replication, synchronization, remote access, or server operation.
- Promising durable commits on remote, virtual, or otherwise untruthful storage stacks.

## Further Notes

- Canonical terminology is defined by the project glossary. In particular, use ordered keyspace, value, Blob, Blob identifier, storage backend, compression policy, portable database file, and provider rather than the discouraged synonyms recorded there.
- Settled architectural constraints are the permanent creation-time storage backend, clean-close definition of the portable database file, header-only boundary for project code, and first-class transactional Blob facility.
- Supporting primary-source research selected the cross-platform durability primitives and Zstandard provider. Its earlier agent-selected AES-256-GCM-SIV and HKDF-SHA-256 recommendations were explicitly superseded by [ADR 0015](../../docs/adr/0015-use-xchacha20-poly1305-ietf.md) and [ADR 0018](../../docs/adr/0018-derive-database-keys-with-blake2b.md); construction-independent authenticated-unit, domain-separation, fail-closed, and threat-boundary conclusions remain inputs.
- Fourteen mapped specification work items cover the public API, transaction contract, durability, backend architecture, portable format, encryption, compression, Blob storage, capacity, recovery, maintenance, and verification. Resolved research items should be incorporated directly; open items must be answered coherently before this specification can authorize database implementation.
- The expected test seam is the public backend-conformance contract, supplemented only by narrow durable-file and provider seams where the relevant external failure cannot be induced at the public boundary.
