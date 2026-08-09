# Recovery, maintenance, and verification contract

Status: v1 frozen

This document is the canonical executable behavior for recovery, maintenance, verification, and v1 qualification. It deepens the [public and transactional contract](./public-transaction-contract.md) over the bytes frozen by the [portable B+ tree and Blob format](./portable-btree-blob-format.md). A conforming implementation may choose private algorithms and caches, but it may not choose a different durable outcome, public failure, maintenance effect, report value, or qualification threshold.

## Scope and safety boundary

V1 has exactly one automatic recovery operation: Crash recovery selects and validates one complete Committed state. It never reconstructs missing state, rewrites bytes during open, or exposes a partial database.

V1 provides no repair, Salvage, or degraded read-only continuation. `verify()` and `verifyFile()` diagnose without mutation. A future separately named offline salvage tool may use Structural metadata, but is not part of the database API and may not weaken normal open behavior.

The B+ tree backend has no data-bearing sidecar. Its authoritative state is always in the main file. A lock sidecar has no recovery meaning. The general public allowance for encrypted recovery sidecars remains available to future backends, whose backend-specific contracts must preserve every public outcome defined here.

The durability promise applies to an already-open, validated local regular file when the filesystem, device, firmware, hypervisor, and host truthfully honor the required stable-storage barrier. V1 cannot detect or correct a storage stack that acknowledges stability falsely. Whole-file replacement by an older internally valid snapshot remains outside the threat model.

## Durable-file model

The implementation's narrow durable-file seam has exact positioned read, exact positioned write, resize, and stable-storage-barrier operations. Writes loop over short successful transfers. Database files are never opened with append semantics. The canonical write path is buffered positioned I/O rather than writable memory mapping.

The platform barrier is `FlushFileBuffers` on Windows, `fsync` on Linux, and `fcntl(F_FULLFSYNC)` on macOS. A completed barrier makes every preceding successful write and required file metadata stable under the stated storage assumptions. Before a completed barrier, writes may be lost, torn, or reordered. A failed or interrupted barrier proves no ordering or stability for the pending phase.

Preallocation is an optional optimization and never evidence of durability. Unsupported preallocation falls back to ordinary writes. Namespace creation and installation use the strongest platform sequence available plus reopen validation, but platforms without a durable parent-directory primitive cannot provide a stronger power-loss namespace promise.

## Stable report contract

The public declarations remain allocator-aware aliases of `Database<Allocator, Limits>`. The following sketches fix their semantic fields; literal header organization may differ without changing names, types, limits, or meanings.

```cpp
enum class VerificationSeverity : std::uint8_t {
    Corruption = 0,
    Observation = 1
};

enum class VerificationExtentRole : std::uint8_t {
    Unknown = 0,
    OrderedInternal,
    OrderedLeaf,
    OverflowValue,
    BlobCatalogInternal,
    BlobCatalogLeaf,
    BlobManifest,
    BlobChunkIndexInternal,
    BlobChunkIndexLeaf,
    BlobChunk,
    AllocatorRoot,
    FreeIndexInternal,
    FreeIndexLeaf,
    RetiredIndexInternal,
    RetiredIndexLeaf
};

enum class VerificationFindingCode : std::uint16_t {
    IncompleteInactivePublication = 1,
    AbandonedTail = 2,

    PublicationConflict = 0x100,
    FileTruncated,
    ExtentOutOfBounds,
    ExtentFramingInvalid,
    ExtentAuthenticationFailed,
    CodecEnvelopeInvalid,
    DecodeFailed,
    CanonicalEncodingInvalid,
    ReferenceMismatch,
    RoleMismatch,
    GenerationMismatch,
    TreeTopologyInvalid,
    TreeOrderingInvalid,
    BlobInvariantInvalid,
    AllocationOverlap,
    AllocationGap,
    AllocationCountMismatch,
    DuplicateReachability
};

enum class RecoveryCause : std::uint8_t {
    None = 0,
    CommitKnownUnpublished,
    CommitOutcomeUnknown,
    MaintenancePersistenceFailed,
    ClosePersistenceFailed,
    ConfirmedCorruption
};

struct VerificationFinding {
    VerificationSeverity severity;
    VerificationFindingCode code;
    std::uint64_t blockIndex;
    std::uint64_t blockCount;
    VerificationExtentRole extentRole;
    std::uint64_t generation;
};
```

`UINT64_MAX` is the unknown value for a block index or generation; an unknown span has block count zero. Existing enum values and meanings are stable, and later API versions may append values only.

`VerificationReport` owns these fields:

- `valid`, true exactly when it has no `Corruption` finding;
- `selectedGeneration`;
- `extentsChecked`, `encodedBytesChecked`, and `decodedBytesChecked`;
- `keysChecked`, `blobsChecked`, and `blobChunksChecked` for objects actually reached;
- `liveBlocks`, `freeBlocks`, `retiredBlocks`, and `abandonedTailBlocks`;
- at most 64 `VerificationFinding` values;
- `findingsTruncated`, true when another finding was discovered after the retained set filled.

Findings are the first 64 in canonical order by severity rank (`Corruption` first), block index with unknown last, numeric code, role, generation, and block count. Verification continues every safely reachable independent branch after the cap. If a failed parent prevents descent, its unreachable descendants are not guessed or counted.

`abandonedTailBlocks` is the ceiling of Abandoned-tail bytes divided by the Allocation quantum; a nonempty partial quantum therefore counts as one block. The exact byte count remains available in diagnostics and `BackupReport`.

Reports and findings never contain application keys, Values, Blob identifiers, content bytes, nonces, tags, paths, encryption material, provider messages, native messages, or timing data.

`BackupReport` owns exactly these fields:

- `sourceGeneration`;
- `destinationFileBytes`;
- `extentsVerified` and `encodedBytesVerified`;
- `liveBlocks`, `freeBlocks`, and `retiredBlocks`;
- `hadIncompleteInactivePublication`;
- `excludedAbandonedTailBytes`.

The incomplete-publication field means that inactive slot bytes were rejected while the selected slot proved a committed generation. It records the recoverable physical condition, not whether interruption or later damage caused it.

`DiagnosticsSnapshot::recoveryCause` uses the stable `RecoveryCause` values above. Its recovery fields are `rejectedInactivePublication`, `abandonedTailBytes`, and the selected generation already exposed as the last known committed generation. `None` is required outside recovery-required state. Existing causes and meanings are stable; later API versions may append causes only.

## Open and crash recovery

### Selection algorithm

`open()` performs these steps in order:

1. Resolve file identity, enforce the one-session registry, and acquire the strongest practical exclusive lock. Locking detects cooperative misuse but does not make unsupported simultaneous processes correct.
2. Read only the fixed visible bootstrap envelope under its canonical bounds. Unknown magic, envelope version, or common format is `UnsupportedFormat`; an unknown required envelope feature or encryption suite is `UnsupportedFeature`; an unsupported KDF or derivation identity is `IncompatibleProfile`.
3. Require the selected cryptographic capabilities and derive the database keys. Missing or operationally failed capabilities are `ProviderUnavailable` and never become authentication rejection.
4. Parse and authenticate both fixed publication slots independently. A structurally invalid, torn, or unauthenticated slot is rejected without releasing plaintext. If neither authenticates under an otherwise supported bootstrap, return `AuthenticationFailed`, intentionally conflating the wrong key with encrypted-header corruption.
5. Canonically validate every authenticated plaintext. Authenticated noncanonical fields, contradictory slots, impossible generation relationships, or unequal semantic content at an equal generation are `Corrupt`. Unknown identities in the newest authenticated publication produce their assigned `UnsupportedFormat`, `UnsupportedFeature`, or `IncompatibleProfile`; they never cause predecessor fallback.
6. Select the highest valid authenticated generation. One authenticating slot is sufficient. Adjacent generations require the newer slot's designated index and predecessor link. A rejected designated newer slot permits selection of the authentic predecessor.
7. Require the physical file to cover the selected committed high-water boundary. Authenticate, decode, and shallowly validate every non-null selected root, including framing, physical reference, role, generation context, canonical root header, and allocator high-water agreement. I/O and provider failures retain their category. Any authenticated-format or reachable-root defect is `Corrupt` and never causes fallback.
8. Record but ignore physical bytes beyond the selected high-water boundary as the Abandoned tail. Establish an in-memory allocator view in which every retired run with no possible surviving process-local snapshot is reusable. Do not write, resize, publish, or issue a barrier.

Open is deliberately not a full verification traversal. A deeper reachable defect discovered later is `Corrupt` and atomically enters recovery-required state under the public contract.

### Reopen outcomes

| Durable state | Reopen outcome |
|---|---|
| Supported bootstrap and neither slot authenticates | `AuthenticationFailed` |
| One slot authenticates; the other is invalid, torn, or unauthenticated | Select the authentic slot |
| Two canonical adjacent slots authenticate | Select the newer generation |
| Two equal slots authenticate with equal semantics | Select that generation |
| Authenticated slots contradict | `Corrupt` |
| Newest authenticated slot requires an unsupported identity | Assigned compatibility error; no fallback |
| Newest authenticated slot has a missing, truncated, unauthenticated, or invalid reachable root | `Corrupt`; no fallback |
| File ends before selected high-water boundary | `Corrupt` |
| File extends beyond selected high-water boundary | Open selected generation and ignore the tail |
| Complete older internally valid file replaces the database | Opens normally; rollback detection is out of scope |

## Creation and initial publication

Creation never exposes a partially initialized requested path:

1. Reject an existing target and create a unique exclusive sibling temporary file.
2. Generate the database identity, salt, initial nonces, bootstrap, and both canonical generation-one publication slots completely before durable output. The two slots have equal semantics, their own physical slot indices, and independent nonces.
3. Write the complete common region and initial backend state, resize to its exact committed boundary, and perform one stable-storage barrier. Any write, resize, provider, space, or barrier failure leaves the requested path absent and removes the temporary file best-effort.
4. Close and reopen-validate the temporary file, then install it exclusively at the requested name and perform the strongest available namespace stabilization.
5. Reopen-validate the requested path before returning the session.

Before exclusive installation, interruption leaves the requested path absent and may leave only an orphan temporary file. After installation, the requested path is absent or a complete validating generation-one database according to platform namespace persistence; it is never a partially initialized file. Failure after installation returns no handle and leaves any complete installed database for explicit `open()` or removal, as frozen by the public contract.

## Transaction publication and interruption

Commit preflight fixes every logical mutation, representation, nonce, allocation, allocator-metadata reservation, byte count, capacity check, and provider result before persistence. Persistence performs no planned allocation.

| Phase | Required action | Failure returned by `commit()` | Reopen boundary |
|---|---|---|---|
| 0 | Preflight only | Original category; writer remains active where the public contract permits | Predecessor; file unchanged |
| 1 | Write candidate extents and allocator state; publication slot untouched | `CommitFailed`; writer terminal, session recovery-required | Predecessor |
| 2 | First stable-storage barrier | `CommitFailed`; writer terminal, session recovery-required | Predecessor |
| 3 | First attempted byte of designated publication-slot write through completion | `CommitOutcomeUnknown`; writer terminal, session recovery-required | Predecessor if slot rejects; candidate if it authenticates and validates |
| 4 | Second stable-storage barrier until successful return | `CommitOutcomeUnknown`; writer terminal, session recovery-required | Predecessor or candidate by authenticated bytes |
| 5 | Barrier succeeded | `commit()` succeeds after non-failing in-memory finalization | Candidate |

The first attempted publication-slot write is the exact uncertainty boundary even when the operating system reports that zero or only some bytes transferred. Failure before it is known unpublished. After it, only recovery can decide.

If a candidate publication authenticates but a referenced unit is damaged, reopen returns `Corrupt` rather than the predecessor. If `std::bad_alloc` unexpectedly escapes after persistence starts, the public contract's terminal-writer and `CommitOutcomeUnknown` rule applies.

## Common maintenance rules

`checkpoint()`, `compact()`, and `backupTo()` join the FIFO writer lane. They never overlap a writer or another maintenance operation. Readers may continue unless the public contract already forbids their operation. `verify()` excludes writers and maintenance but may coexist with readers.

Checkpoint, compaction, and close-time checkpoint preflight complete all fallible planning, provider work, allocation, limit checks, and optional definitive preallocation before the first source-file mutation. A non-corruption preflight failure leaves handles, generation, and bytes unchanged. Positively established corruption always follows the public corruption transition regardless of phase. Once the first source write or resize begins, any other source-side failure enters `RecoveryRequired` while retaining its primary `Io`, `Durability`, `ProviderUnavailable`, or `ResourceLimit` category. `CommitFailed` and `CommitOutcomeUnknown` are reserved for transaction commit.

For a maintenance root publication, slot-phase reopen outcomes are identical to transaction publication. After its second barrier succeeds, the maintenance generation is authoritative. Failure during later physical tail truncation leaves that generation authoritative and may leave an Abandoned tail.

No maintenance workflow implicitly changes format, backend, encryption suite, compression policy, capacity profile, application content, Blob identity, or whole-file rollback behavior.

## Checkpoint

For the B+ tree backend, checkpoint is a reclamation checkpoint:

1. Capture the selected generation and the oldest live snapshot generation while holding writer admission. A retired run at retirement generation `r` is eligible exactly when no live snapshot has generation below `r`; with no readers, every run through the selected generation is eligible.
2. Move every eligible run to the candidate free index and coalesce free runs into canonical maximal nonadjacent ranges.
3. Build and preflight the replacement allocator metadata. New allocator pages superseded by the operation follow the normal retirement-generation rule.
4. If allocator semantics changed, publish one generation using the ordinary two-barrier protocol. If they did not, publish nothing.
5. Resize away physical bytes beyond the now-selected committed high-water boundary and stabilize the resize. This removes only an Abandoned tail, never a free suffix within the committed boundary.

Checkpoint does not relocate reachable extents, lower the committed high-water mark, or traverse content for full verification. If allocator state is unchanged and physical length already equals the committed boundary, it is a no-write, no-barrier no-op.

## Compaction

Compaction is online, in-place, snapshot-safe copy-on-write relocation:

1. Capture the selected roots, eligible retired runs, and source physical length under writer admission. Reclassify eligible retired runs into the planned free set exactly as checkpoint does.
2. Authenticate every extent needed to plan relocation. Confirmed corruption follows the ordinary corruption transition.
3. Compute a bounded fixed-point plan using lowest-address first-fit destinations. Current reachable extents are considered from highest source block downward; child-bearing structures are rebuilt bottom-up whenever a moved child changes a direct reference. A move is retained only when its final destination is below its source and advances the lowest-address layout. Ties use physical block, extent role identifier, and encoded reference bytes.
4. The plan includes every rewritten ancestor, allocator page, fresh nonce, encoded representation, temporary appended span, retained-reader span, resulting partition, and candidate high-water mark. Failure to prove sufficient profile capacity and workspace is preflight `ResourceLimit`.
5. Immediately before sealing allocator metadata, briefly close reader admission, capture and internally pin every existing snapshot generation, and finalize retained ranges. Existing readers continue; new readers wait until publication succeeds or fails. This gate prevents a reader from acquiring the predecessor after the plan has omitted its extents.
6. If either allocator semantics or physical placement changed, write the plan without modifying predecessor-reachable extents, perform the first barrier, and publish exactly one compacted generation through the designated slot and second barrier. Otherwise publish nothing.
7. Extents needed by the pinned older snapshots remain retired below the candidate high-water mark. Without such readers, relocated source spans below the boundary become free and a now-unreachable suffix lies outside the candidate boundary. Release reader admission after the publication outcome has been recorded.
8. After publication is durable, or immediately when none was needed, resize to the selected candidate high-water boundary and stabilize removal of any Abandoned tail. Interruption here reopens the selected generation and treats any remaining suffix as abandoned.

Compaction never waits for existing readers, expires a snapshot, or invalidates a handle. Its immediate shrinkage is bounded by the oldest reader. If neither reclamation nor relocation can advance the snapshot-safe layout and no tail exists, it succeeds as a no-op; operators use diagnostics, end readers, and invoke it again. It never builds or renames a sibling replacement database.

## Portable backup

`backupTo(destination)` creates a verified physical snapshot:

1. Reject an existing destination without opening, truncating, adopting, or replacing it.
2. Hold the writer lane and capture one selected source generation. Readers may continue.
3. Perform the offline-authoritative verification scope below for that generation and allocator partition. Source corruption follows the online corruption transition. A source I/O or provider failure retains its category but does not by itself poison the source session.
4. Create a cryptographically unique exclusive sibling temporary file in the destination directory.
5. Copy exact source bytes from offset zero through `high_water_blocks * allocationQuantumBytes`. Preserve bootstrap, both publication slots, database identity, generation, nonces, extent positions, free and retired representation, and every other byte in the committed prefix. Do not copy sidecars or the Abandoned tail and do not compact, reclaim, normalize, or re-encrypt.
6. Stabilize and close the temporary file, reopen it with the session's providers and keys, and repeat bounded open validation.
7. Install it atomically and exclusively at the requested name, reopen-validate the final path, and apply the strongest available parent-directory stabilization.

Before installation, failure leaves the requested destination absent and removes the temporary file best-effort. After installation, validation or namespace-durability failure throws `Durability` and leaves the complete destination for explicit verification or removal. Interruption may leave an orphan uniquely named temporary file, but the requested path is absent or a complete backup, never a partially copied file. Destination-side failure never changes or poisons the source.

Success returns the frozen `BackupReport`. A smaller backup requires explicit compaction before backup.

## Integrity verification

### Authoritative scope

Online `verify()` validates the selected committed graph and every older graph retained by a live read snapshot. At admission it internally pins the then-live snapshot roots; because writers are excluded, a later reader observes the already-pinned selected generation. Offline `verifyFile()` validates the selected committed graph; no process-local snapshot survives offline.

Both validate:

- bootstrap and publication selection, recording a rejected inactive slot as an observation;
- framing, bounds, authentication, role, owner context without reporting identity, generation relationships, codec envelope, bounded decompression, and canonical decoded bytes for every reachable extent;
- all B+ tree topology, levels, child counts, separators, key ordering, prefix compression, slot packing, entry schema, empty-root, split-independent canonical representation, and reference invariants;
- ordered-keyspace Value representation and overflow length invariants;
- Blob catalog uniqueness, manifest identity and length, dense chunk ordinals, index ownership, content generation, chunk size, and final-chunk length;
- free and retired ordering, coalescing, disjointness, retirement eligibility representation, complete high-water partition, duplicate reachability, and authenticated counters;
- physical file length relative to the committed high-water boundary.

They classify but do not authenticate or interpret payload bytes in free runs, offline-obsolete retired runs, or the Abandoned tail. Those ranges have no authoritative content. Online retired extents still reachable by a live snapshot are authenticated through that snapshot's graph.

### Outcomes

Structural or authentication defects discovered after a publication establishes identity are retained as `Corruption` findings rather than thrown merely to report the first defect. Verification safely continues independent branches, returns an invalid report, and online verification atomically enters `RecoveryRequired` and invalidates subordinate handles before returning. Offline verification has no session to mutate.

`verifyFile()` uses the same publication authentication and selection rules as open, but after one publication establishes identity it converts selected-root truncation, framing, authentication, and structural failure into report findings so the offline diagnostic operation can return an invalid report. Normal `open()` continues to throw `Corrupt` for the same selected-root condition. A defect that prevents bounded bootstrap parsing or leaves no authentic publication follows the bootstrap outcomes rather than fabricating a report.

An incomplete inactive slot and Abandoned tail are `Observation` findings and do not make the report invalid. Wrong key or inability to establish any encrypted publication makes `verifyFile()` return `AuthenticationFailed`. I/O, allocation, unavailable-provider, and operational-provider failures throw because verification could not decide validity. They do not poison an online session unless corruption was positively established.

`verify()` and `verifyFile()` never write, repair, truncate, reclaim, normalize, or salvage.

## Close and destruction

After the public close-admission and live-child rules succeed, ordinary B+ tree close performs checkpoint semantics, then erases session keys best-effort, releases locks, closes resources, and becomes `Closed`. It never compacts or performs full verification. With no checkpoint work, it writes and barriers nothing.

Close from `RecoveryRequired` is cause-dependent:

- For commit, maintenance, or close persistence uncertainty, re-run byte-preserving slot selection and shallow root validation. If one complete generation is established, perform checkpoint-on-close and finish normally.
- If that resolution encounters I/O, provider failure, or corruption, throw the primary error, remain recovery-required, and permit another explicit attempt. Destruction still releases resources best-effort.
- For already-confirmed corruption, perform resource-only shutdown without content traversal, checkpoint, publication, truncation, repair, or a claim that validity was restored. Transition to `Closed` unless resource closure itself fails.

A resource-close failure retains the primary `Io` or `Durability` category and the `RecoveryRequired` state so another explicit close attempt or destruction can finish releasing resources. The durable-file adapter must retain or reacquire enough exact file identity and lock state to make that retry safe; it may not report a reusable `Open` session after a partial close.

Destruction retains the non-throwing behavior frozen by the public contract. A resource-only close or destruction never turns a corrupt file into a valid one; later open fails closed at the same authoritative defect.

## Insufficient space and allocation failure

- A profile limit, impossible allocator fixed point, inadequate compaction workspace, or definitive preallocation exhaustion before mutation is `ResourceLimit` and has no effect.
- Runtime native out-of-space errors retain their native code. During transaction persistence they map to `CommitFailed` before publication attempt and `CommitOutcomeUnknown` afterward. During source-mutating maintenance they are `ResourceLimit` and enter recovery-required state. During destination-only backup or creation they are `ResourceLimit` and do not affect an existing source.
- `std::bad_alloc` remains memory exhaustion and never becomes `ResourceLimit`.
- No workflow implicitly compacts, deletes a backup, overwrites a destination, discards committed content, weakens durability, or continues beyond the capacity profile to obtain space.
- Byte-preserving open requires no free storage.

## V1 qualification gates

Release requires every gate below with zero unresolved failures. A minimized failure corpus or history becomes a permanent regression asset.

### Backend conformance and reference model

One backend-independent in-memory model covers the ordered keyspace, Values, transactional Blobs, snapshots, writer admission, cursor invalidation, limits, and terminal handles. Tests use only the public API and never assert private pages, allocation order, provider call sequence, or cache internals.

- Exhaustively enumerate histories through depth eight over three byte-string keys, empty/nonempty Values, two Blobs, commit/rollback, reads, erases, and cursor movement.
- Run 1,000 fixed seeds of 10,000 operations including boundary sizes, chunk transitions, snapshot overlap, maintenance, close/reopen, expected misuse, and uncertain persistence.
- Compare every result, exception category, valid-handle observation, full committed keyspace, and Blob content with the model after each operation and reopen boundary.
- Run the identical suite for every claimed backend, capacity-profile boundary, and provider combination.

Passing permits no mismatch, unexpected exception, leaked resource, or model-forbidden state.

### Deterministic durability and fault injection

Instrument every durable-file write, resize, and barrier. Minimal fixtures interrupt before and after every operation; inject short or torn writes at zero, one, native-sector boundaries, Allocation-quantum boundaries, and the final byte, plus seeded additional points. Before a successful barrier, permit loss, partial retention, and reordering. A failed barrier may leave any preceding unbarriered subset stable but the implementation may not enter the next phase.

Exhaustively enumerate publication-slot sector subsets for both slots. Exercise transactions, checkpoint, compaction, close-time checkpoint, creation, and backup installation. Reopen every resulting file through the public API.

Passing requires successful operations to survive; pre-publication interruption to select the predecessor; publication interruption to select predecessor or complete candidate; damage beneath an authenticated candidate to return `Corrupt`; and no mixed state, unauthenticated plaintext, silent stale fallback, or unexpected writable session. Minimal deterministic campaigns run on all supported targets; larger seeded campaigns run continuously.

### Corruption and authentication

Version-controlled byte fixtures cover empty, multi-level, overflow-Value, multi-Blob, compressed, fragmented, retired, checkpointed, compacted, and backed-up files. Mutate every visible and authenticated field at zero, one-bit, boundary, unknown-identifier, and noncanonical-reserved values. For every extent role, mutate preamble, ciphertext, tag, padding, compressed frame, decoded canonical field, and parent reference. Test truncation at every common-region byte and every framing, ciphertext, tag, quantum, and high-water boundary, plus duplication, relocation, swapping, replay, cross-role, cross-Blob, and cross-database substitution.

Passing requires the exact compatibility, `AuthenticationFailed`, predecessor-selection, `Corrupt`, or ignored-residue outcome defined above; sentinel output proves authentication or decode failure releases no plaintext. Missing capabilities and operational provider failures remain distinct. There may be no crash, hang, overread, uncontrolled allocation, or sensitive diagnostic. Whole-file rollback remains accepted by design.

### Portability and providers

The release matrix is Windows, Linux, and macOS on x86-64 and ARM64. Each of the six targets produces fixtures that every target must open, verify, mutate, close, back up, and reopen. Fixtures span compression `None` and `ZStd`, every permitted Allocation quantum and Blob chunk size, zero/default/maximum inline cutoff, all structural states listed above, unsupported identities, and permitted interrupted publication.

Canonical KDF, AEAD, associated-data, page, codec-decode, and format fixtures are byte-exact. Vendored and system `libzstd` builds cross-read; compressed output need not match, but decoded content and the allocation-quantum savings decision must. Cryptography passes upstream libsodium vectors and an independent BLAKE2b/XChaCha fixture implementation. Missing capabilities never silently substitute. No platform or provider update ships if canonical bytes change or any prior fixture loses readability.

### Fuzzing and sanitizers

Dedicated targets cover public histories, bootstrap/publication selection, extent framing/authentication, every page role, Blob manifests and chunk indexes, allocation partitions, Zstandard validation, cursor/range boundaries, and recovery/maintenance interruptions. Seed corpora reach every specified field, role, finding code, and error branch. Inputs and provider outputs are explicitly bounded.

Before release, every parser/recovery target completes 24 CPU-hours and the stateful history target completes 72 CPU-hours from the merged permanent corpus. Full deterministic and randomized suites pass under ASan+UBSan; concurrency passes separately under TSan; MSan runs on supported Clang/Linux and the supported Windows address sanitizer runs there. Every crash, hang, sanitizer result, assertion, or uncontrolled resource case is minimized and retained. Release requires zero unresolved results and corpus replay on all six targets.

### Concurrency

A deterministic scheduler exhaustively explores bounded reader creation/end, writer admission, publication, rollback, cursor and Blob reads, maintenance, corruption publication, and close. Stress uses 256 readers, one writer, long snapshots, reclamation pressure, cache eviction, and queued maintenance.

Explicit races cover FIFO ordering and close cancellation, readers crossing publication, snapshot-safe reuse, online compaction, verification with readers, corruption invalidation, commit-uncertainty reader preservation, destruction/moves, and wrong-thread rejection. Every read observes one complete modeled generation.

Each release target runs two hours; Linux x86-64 and ARM64 additionally run 24 hours under TSan. Passing permits no race, deadlock, lock-order failure, use-after-free, model mismatch, FIFO inversion, lost wakeup, or unexplained starvation. Admitted work finishes unless a documented retained handle or injected fault blocks it.

### Performance

Performance uses a checked-in corpus on published per-platform reference desktops: release build, native local filesystem, at least eight hardware threads, 16 GiB RAM, and NVMe storage honoring barriers. Report five runs after one warm-up; medians and p99 values must satisfy the following default-profile gates:

| Metric | Gate |
|---|---:|
| Shallow healthy open | p99 <= 100 ms, independent of database size |
| Cache-hit point lookup | p99 <= 200 microseconds |
| Random on-disk point lookup | p99 <= 5 ms |
| Durable transaction, 100 small mutations | p99 <= 50 ms |
| Ordered scan | >= 100 MiB/s decoded payload |
| Sequential Blob read and write | >= 200 MiB/s logical payload |
| Full verification and physical backup | >= 150 MiB/s physical input |
| Reader-free compaction | >= 75 MiB/s relocated live data |
| 256-reader cache-hit workload | >= 50,000 operations/s and p99 <= 5 ms |
| Database-owned peak memory | configured cache + 128 MiB or less |
| Reader-free post-compaction file size | <= 1.25 times profile-rounded live representation |
| Steady file size without retained snapshots | <= 3 times live representation before compaction |
| No-op checkpoint | no writes/barriers and p99 <= 1 ms |

The corpus includes small and overflow Values, compressible and incompressible pages, 1 MiB through 10 GiB Blobs, random updates, scans, and long snapshots. Provider-owned memory and returned application buffers are excluded from the database-owned peak. A regression greater than 15 percent in throughput, p99 latency, peak memory, or amplification against the recorded baseline blocks release even when the absolute floor passes.

These performance promises apply only to the default capacity profile and representative desktop workload, not the absolute correctness limits.
