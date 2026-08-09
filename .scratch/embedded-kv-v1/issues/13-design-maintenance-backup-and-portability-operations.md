# Design maintenance, backup, and portability operations

Type: grilling
Status: resolved
Blocked by: 05, 10, 12

## Question

What explicit operations should v1 provide for checkpointing sidecars, compacting or reclaiming space, creating a consistent backup while open, verifying integrity, cleanly closing, and producing or recognizing a safely movable portable database file, including interruption and free-space requirements?

## Resolution

Resolved by the frozen [recovery, maintenance, and verification contract](../../../docs/recovery-maintenance-verification-contract.md) and indexed by [implementation ticket 03](../../embedded-kv-v1-implementation/issues/03-freeze-recovery-maintenance-and-verification.md).

For the sidecar-free B+ tree backend, `checkpoint()` durably persists snapshot-safe reclamation and abandoned-tail removal without relocating live extents. `compact()` is online, bounded by retained snapshots, and uses one in-place copy-on-write publication. `backupTo()` verifies and copies the authoritative committed prefix to a new non-overwritten destination; `verify()` and `verifyFile()` diagnose authoritative state without mutation, repair, or salvage. Clean close runs checkpoint semantics but neither compaction nor full verification. Preflight resource failure has no effect; source mutation failure enters recovery-required state with its primary error, while destination-only backup failure leaves the source unchanged. ADRs [0046](../../../docs/adr/0046-make-btree-checkpoint-persist-reclamation.md) through [0057](../../../docs/adr/0057-keep-backup-reports-physical-and-minimal.md) freeze the detailed workflows and reports.
