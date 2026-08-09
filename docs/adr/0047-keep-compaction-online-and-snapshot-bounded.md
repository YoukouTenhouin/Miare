# Keep compaction online and snapshot-bounded

`compact()` preserves active readers and relocates only within the space permitted by their retained snapshots; it never waits for reader quiescence, expires snapshots, or invalidates handles. A compacted generation may therefore be published without an immediate file-size reduction, and a call with no useful snapshot-safe reclamation, relocation, or tail removal is a successful no-op; diagnostics expose retained bytes so an operator can end readers and compact again rather than risking an indefinitely blocked synchronous operation.
