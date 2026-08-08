# Store large blobs through a distinct transactional facility

Large media assets must remain inside the portable database file so applications retain single-file portability and unified encryption and compression. V1 will therefore expose first-class transactional blobs with incremental I/O and chunked storage, while keeping ordinary values bounded and in-memory; values may hold database-local blob identifiers, and blob creation or removal participates atomically in the surrounding transaction.

A Blob identifier names mutable database-local object identity rather than immutable content. A write transaction may stream replacement content under the same identifier; existing snapshots retain the old content, and commit or rollback publishes or discards the replacement atomically. The database neither discovers nor enforces references from values to Blob identifiers, leaving referential integrity to the embedding application.

Blob readers are seekable snapshot streams so large media can be consumed incrementally or accessed at an absolute offset. Creation and replacement writers are sequential from byte zero and require explicit finalization; append-specific, sparse, truncate, and partial-update APIs are excluded because applications can transactionally replace the comparatively rare content changes.
