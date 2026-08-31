# Operations, errors, and recovery

Miare distinguishes legitimate alternative outcomes from failures to fulfill an
operation. It also exposes synchronous maintenance and diagnostics for
application worker threads.

## Alternatives and exceptions

`Result<T, E>` carries only a documented alternative:

- `open()` and `verifyFile()` return `AuthenticationFailed` when no encrypted
  publication can authenticate. This intentionally does not distinguish a
  wrong key from bootstrap tampering.
- Keyless `openUnencrypted()` and `verifyUnencryptedFile()` return suite-0
  corruption as stable findings or `Errc::Corrupt`; they never imply
  cryptographic authentication.
- `tryBeginWrite()` returns `WriterBusy` when immediate fair admission is not
  available.
- `get()`, `openBlob()`, and `replaceBlob()` use empty `std::optional` for
  ordinary absence.

Contract mistakes throw `ContractError`, derived from `std::logic_error`.
Operational failures throw `DatabaseError`, derived from `std::runtime_error`.
Both expose a stable `Errc`; only `DatabaseError` can carry an underlying
`std::error_code`. Do not parse `what()` text for control flow.

```cpp
try {
    auto database = miare::Database<>::create(
        path, key, miare::ProviderSet::system());
    // ...
} catch (const miare::ContractError& error) {
    reportProgrammingError(error.code());
} catch (const miare::DatabaseError& error) {
    reportOperationalFailure(error.code(), error.nativeCode());
}
```

`std::bad_alloc` remains outside this hierarchy.

## Recovery-required state

Commit persistence uncertainty, maintenance persistence failure, close
persistence failure, or confirmed corruption can move an open session to
`DatabaseState::RecoveryRequired`. New transactions and mutations are then
rejected. `diagnostics().recoveryCause` identifies the stable cause.

Existing safe read snapshots may finish after persistence uncertainty.
Confirmed corruption invalidates subordinate handles immediately. Close and
reopen the database to run deterministic recovery; Miare never repairs,
salvages, or partially exposes invalid content.

## Diagnostics

`diagnostics()` returns an internally consistent snapshot containing format and
profile identity, storage sizes, cache pressure, active readers, writer queue
state, reclaimable and snapshot-retained bytes, and recovery state. It contains
no application keys, values, Blob identifiers, paths, or encryption material.

Long-lived readers are visible through `activeReaders`,
`oldestReaderGeneration`, `oldestReaderAge`, and `snapshotRetainedBytes`. Use
these fields to explain delayed reclamation rather than expiring snapshots.

## Checkpoint and compaction

`checkpoint()` makes currently safe reclamation durable and removes an
abandoned physical tail. It does not relocate reachable content.

`compact()` relocates current content toward lower addresses and publishes one
snapshot-safe generation. It does not wait for or invalidate readers, so an old
snapshot may limit immediate shrinkage. End unneeded readers and compact again
when diagnostics show retained bytes.

Both operations are synchronous, join the FIFO writer lane, and may be no-ops.
V1 has no progress callback, cancellation, or asynchronous maintenance API.

## Portable backup

`backupTo(destination)` creates a verified physical snapshot of one committed
generation. It preserves database identity, encryption suite, compression,
nonces or checksums, layout, and generation; it is not an export or logical
rebuild. A backup has exactly the source file's confidentiality and integrity
properties.

The destination must not exist. Failure before installation leaves it absent;
a namespace-durability failure after installation may leave a complete backup
for explicit verification or removal. Destination-side failure does not poison
the source session. Compact explicitly before backup when a smaller physical
snapshot is desired.

## Verification

`verify()` checks the selected committed state plus generations retained by
current readers and returns a bounded `VerificationReport`. Suite 1
cryptographically authenticates protected units; suite 0 validates unkeyed
checksums and structure only. Corruption findings make `valid` false and place
the online session in recovery-required state; observation findings such as an
abandoned tail do not.

`Database<>::verifyFile(path, key, providers)` performs offline verification
without modifying the file. Bootstrap rejection returns `AuthenticationFailed`;
once identity is established, structural defects are returned as findings.
Use `verifyUnencryptedFile(path, providers)` for a keyless file.

Verification never exposes application content in findings and never writes,
repairs, truncates, normalizes, or salvages the database.

Checksums detect accidental corruption but are publicly recomputable. For an
unencrypted database, a valid report is not evidence against malicious
modification, does not authenticate the file's origin, and provides no
confidentiality. Compression changes representation, not any security
guarantee.

## Provider deployment

Configure `MIARE_ENABLE_SODIUM` and `MIARE_ENABLE_ZSTD` independently. A
disabled provider is absent from the installed package's headers and link
interface. At runtime, `ProviderSet::none()`, `systemCrypto()`,
`systemCompression()`, and `system()` expose none, one, or both capabilities.
A file that requires a capability missing from its provider set fails with
`Errc::ProviderUnavailable`; Miare never silently substitutes a different
suite or codec. `Compression::None` performs no compression-provider operation,
and suite 0 performs no crypto-provider operation.

## Shutdown and portability

`close()` rejects live transactions, cursors, or Blob streams with
`Errc::LiveChildren` and leaves them valid. After admission it performs
checkpoint semantics, consolidates committed state into the main file, erases
session keys best-effort, and releases resources.

A successful explicit close is the boundary at which the single database file
is guaranteed portable without data-bearing sidecars. Move that file only after
close. Destruction is non-throwing best effort and cannot report whether this
guarantee was established.

For exact interruption and durability behavior, use the
[recovery, maintenance, and verification contract](recovery-maintenance-verification-contract.md).
