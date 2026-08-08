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
The stable database-local identity by which a value refers to a blob.
_Avoid_: File path, external URL

**Storage backend**:
The persistence strategy chosen when a database is created. It owns backend-specific storage behavior, including the physical compression unit, and cannot be changed for an existing database.
_Avoid_: Engine, runtime backend switch

**Compression policy**:
The database-creation choice that permits transparent compression by the selected storage backend. The backend determines the physical compression unit.
_Avoid_: Compression codec callback, application-managed compression

**Portable database file**:
The single file that contains a database's complete committed state after a clean close and can be moved without auxiliary files. Recovery after an unclean shutdown may additionally require encrypted journal or write-ahead-log files.
_Avoid_: Sidecar-free database, database directory

**Provider**:
A vetted external implementation used for a capability such as cryptography or compression. Providers may be vendored or linked separately and are not required to be header-only.
_Avoid_: Reimplemented primitive, zero-dependency guarantee
