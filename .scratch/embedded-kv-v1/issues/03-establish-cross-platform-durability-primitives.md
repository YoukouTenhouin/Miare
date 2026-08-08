# Establish cross-platform durability primitives

Type: research
Status: resolved

## Question

What guarantees and failure caveats do current primary platform documentation provide for file creation, positioned I/O, memory mapping, atomic replacement, allocation, flushing file data and metadata, directory durability, locking, sparse files, and crash behavior on supported Windows, Linux, and macOS filesystems, and what portable abstraction can truthfully support a durable commit contract?

## Resolution

Adopt a local-regular-file `durable_file` abstraction with exact positioned reads/writes, resize, and an explicit stable-storage barrier: `FlushFileBuffers` on Windows, `fsync` on Linux, and `F_FULLFSYNC` on macOS. A commit must write recovery/new data, barrier, publish a self-validating generation record, then barrier again; recovery must tolerate short and torn writes, reordering before a completed barrier, and an interrupted barrier. Writable mappings require a mapped-range flush plus the same file barrier and are therefore excluded from the canonical v1 write path. Rename/replacement, close, filesystem journaling, sparse files, and preallocation are not commit primitives. Namespace operations get the strongest available platform sequence and reopen validation, while the durable-commit promise is scoped to an already-open validated database on local storage whose filesystem/device stack truthfully honors flushes. Locking is best-effort defensive detection because multi-process access is out of scope.

Research asset: [Cross-platform durability primitives](../research/cross-platform-durability-primitives.md)
