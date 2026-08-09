# Embedded KV Database

An embedded, transactional key-value database intended to be linked directly into desktop applications.

## Language

**Ordered keyspace**:
The set of arbitrary byte-string keys in a database, arranged lexicographically by unsigned byte value. It supports exact lookup, cursors, prefix scans, and bounded range scans.
_Avoid_: Table, collection, custom-ordered keyspace

**Value**:
An arbitrary byte string associated with a key. Interpretation and serialization belong to the embedding application.
_Avoid_: Record, document, object

**Inline value**:
A Value whose complete bytes reside in its B+ tree leaf entry according to the database's capacity profile. Larger Values use an overflow representation without changing their public meaning.
_Avoid_: Small value, embedded object

**Blob**:
A potentially large byte sequence held inside the portable database file and accessed incrementally within a transaction. A blob is distinct from an in-memory value and can be referenced by an identifier stored in a value.
_Avoid_: Large value, external asset, media file

**Blob identifier**:
The stable database-local identity of a Blob. Blob content may be transactionally replaced without changing this identity, while relationships between values and Blob identifiers belong entirely to the embedding application.
_Avoid_: File path, external URL

**Blob chunk**:
One independently authenticated and optionally compressed contiguous portion of a Blob. Its logical size is fixed by the database's capacity profile except for the final chunk.
_Avoid_: Blob page, stream buffer, file block

**Storage backend**:
The persistence strategy chosen when a database is created. It owns backend-specific storage behavior, including the physical compression unit, and cannot be changed for an existing database.
_Avoid_: Engine, runtime backend switch

**B+ tree backend**:
The v1 Storage backend, organized as immutable committed generations of an ordered B+ tree and its associated Blob and allocation state. A new generation replaces affected paths through copy-on-write publication while retained snapshots continue to observe earlier generations.
_Avoid_: B-Tree, in-place tree

**Extent reference**:
The authenticated physical address and bounds of one immutable unit in the portable database file. References are embedded directly in parent structures rather than resolved through a separate object-location map.
_Avoid_: Page ID, object pointer, file pointer

**Allocation quantum**:
The smallest addressable span of backend-owned file space. It is fixed by the database's capacity profile, and extent positions and allocation spans are expressed in whole quanta.
_Avoid_: Filesystem block size, page size, sector size

**Authenticated extent**:
A self-framing immutable unit whose physical bounds, role, generation, representation, and content are validated together before its plaintext is exposed. Parent structures also carry the expected Extent reference and role.
_Avoid_: Raw page, trusted record, allocation block

**Framed-page ceiling**:
The maximum physical span of one B+ tree page including its authenticated framing. It is derived from the Allocation quantum and may bound compressed pages to a smaller whole-quantum span.
_Avoid_: Plaintext page size, cache page size

**Compression policy**:
The database-creation choice that permits transparent compression by the selected storage backend. The backend determines the physical compression unit.
_Avoid_: Compression codec callback, application-managed compression

**Encryption suite**:
The authenticated-encryption construction fixed when a database is created and identified by its portable format. It cannot be changed for an existing database.
_Avoid_: Runtime cipher, provider algorithm

**Encryption key material**:
The high-entropy secret supplied by the embedding application as the root of a database's encryption.
_Avoid_: Password, stored database key

**Structural metadata**:
The visible physical facts needed to classify and bound the portable file's authenticated units, including their roles, sizes, generations, compression representation, and Blob ownership. It excludes application keys, Values, and Blob content.
_Avoid_: Plaintext content, deniable metadata

**Portable database file**:
The single file that contains a database's complete committed state after a clean close and can be moved without auxiliary files. Recovery after an unclean shutdown may additionally require encrypted journal or write-ahead-log files.
_Avoid_: Sidecar-free database, database directory

**Provider**:
A vetted external implementation used for a capability such as cryptography or compression. Providers may be vendored or linked separately and are not required to be header-only.
_Avoid_: Reimplemented primitive, zero-dependency guarantee

**Committed state**:
Database state made atomic and durable by a successfully completed transaction commit, or established as committed by recovery after an uncertain commit outcome.
_Avoid_: Flushed state, saved state

**Uncommitted changes**:
Tentative key, value, and Blob changes owned by an active write transaction. They may be discarded for any reason until their commit succeeds and are not themselves retryable database state.
_Avoid_: Volatile operations, pending committed data

**Read transaction**:
A transaction that observes one stable committed snapshot and cannot create uncommitted changes.
_Avoid_: Const transaction, read-only database

**Write transaction**:
A transaction that owns uncommitted changes and may make them durable through commit.
_Avoid_: Mutable transaction, write lock

**Recovery-required state**:
The state of an open database session that cannot safely accept new database access until explicit recovery.
_Avoid_: Poisoned database, degraded mode

**Crash recovery**:
Deterministic selection and validation of one complete Committed state after an interrupted persistence operation. It never reconstructs, repairs, or partially exposes database content.
_Avoid_: Repair, salvage, best-effort recovery

**Salvage**:
Best-effort extraction of partial content when no complete Committed state can be validated. Salvage is not a v1 database operation.
_Avoid_: Crash recovery, repair, degraded read

**Abandoned tail**:
Physical bytes at or beyond the selected Committed state's high-water boundary. They are not part of any committed generation and may be ignored, overwritten, or removed without salvage.
_Avoid_: Corrupt data, recovery log, free extent

**Checkpoint**:
Maintenance that makes currently safe reclamation durable and removes an Abandoned tail without relocating live content. It may consolidate recovery state for a Storage backend that uses data-bearing sidecars.
_Avoid_: Compaction, verification, backup

**Compaction**:
Snapshot-safe maintenance that relocates the latest Committed state toward lower physical addresses so the committed high-water boundary can shrink. Its immediate result is bounded by content retained for live snapshots.
_Avoid_: Checkpoint, vacuum, logical rewrite

**Portable backup**:
A verified physical snapshot ending at one selected Committed state's high-water boundary and requiring no sidecars. It preserves database identity and physical representation rather than logically rebuilding or compacting content.
_Avoid_: Export, replica, compacted copy

**Integrity verification**:
Validation of every authenticated structure reachable from authoritative Committed state, plus the allocation partition that bounds it. It does not assign meaning to bytes in free space, obsolete retirement state, or an Abandoned tail.
_Avoid_: Raw byte scan, repair, salvage
