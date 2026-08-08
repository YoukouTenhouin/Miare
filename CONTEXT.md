# Embedded KV Database

An embedded, transactional key-value database intended to be linked directly into desktop applications.

## Language

**Ordered keyspace**:
The set of arbitrary byte-string keys in a database, arranged lexicographically by unsigned byte value. It supports exact lookup, cursors, prefix scans, and bounded range scans.
_Avoid_: Table, collection, custom-ordered keyspace

**Value**:
An arbitrary byte string associated with a key. Interpretation and serialization belong to the embedding application.
_Avoid_: Record, document, object

**Blob**:
A potentially large byte sequence held inside the portable database file and accessed incrementally within a transaction. A blob is distinct from an in-memory value and can be referenced by an identifier stored in a value.
_Avoid_: Large value, external asset, media file

**Blob identifier**:
The stable database-local identity of a Blob. Blob content may be transactionally replaced without changing this identity, while relationships between values and Blob identifiers belong entirely to the embedding application.
_Avoid_: File path, external URL

**Storage backend**:
The persistence strategy chosen when a database is created. It owns backend-specific storage behavior, including the physical compression unit, and cannot be changed for an existing database.
_Avoid_: Engine, runtime backend switch

**Compression policy**:
The database-creation choice that permits transparent compression by the selected storage backend. The backend determines the physical compression unit.
_Avoid_: Compression codec callback, application-managed compression

**Encryption suite**:
The authenticated-encryption construction fixed when a database is created and identified by its portable format. It cannot be changed for an existing database.
_Avoid_: Runtime cipher, provider algorithm

**Encryption key material**:
The high-entropy secret supplied by the embedding application as the root of a database's encryption.
_Avoid_: Password, stored database key

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
