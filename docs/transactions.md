# Transactions and ordered scans

Miare has distinct read and write transactions. They are move-only handles,
cannot be nested or upgraded, and remain bound to the thread that created them.

## Snapshot behavior

`beginRead()` captures one committed generation. That snapshot never advances,
so repeated reads remain stable even while another thread commits. Many read
transactions may coexist, but long-lived snapshots delay storage reclamation.

Only one writer or writer-lane maintenance operation is admitted at a time:

```cpp
auto write = database.beginWrite(); // waits in the FIFO writer queue

auto attempted = database.tryBeginWrite();
if (!attempted) {
    // WriterBusy: do something else rather than wait.
}
```

`tryBeginWrite()` does not jump queued work. A write transaction sees its own
key changes and finalized Blob changes. Other readers see them only if created
after the successful commit.

## Exact operations

Both transaction types provide `get()`, `contains()`, `scan()`, and
`openBlob()`. A write transaction additionally provides `put()`, `erase()`, and
Blob mutations.

```cpp
auto write = database.beginWrite();
write.put(bytes("status"), bytes("ready")); // inserts or replaces
const bool removed = write.erase(bytes("obsolete"));
write.commit();
```

`get()` returns `std::optional<Database<>::OwnedBytes>` and `erase()` reports
whether the key existed. Empty keys and values are valid. There are no
insert-only, compare-and-swap, conditional-write, or nested-transaction
operations in V1.

Each successful mutating call counts against the capacity profile even when it
replaces identical bytes. `WriteTransaction::stats()` reports mutation counts,
Blob bytes written, open Blob writers, and estimated file growth.

## Ordered ranges

Keys use unsigned-byte lexicographic order. A scan accepts one of:

```cpp
miare::KeyRangeView::all();
miare::KeyRangeView::prefix(bytes("person:"));
miare::KeyRangeView::halfOpen(lowerInclusive, upperExclusive);
```

Half-open bounds are optional, so either side may be unbounded. Range bytes are
borrowed only during `scan()` and copied into the returned cursor. An empty
prefix means the entire keyspace.

@anchor cursor_navigation
## Cursor navigation

Read and write cursors have the same navigation surface:

```cpp
auto cursor = read.scan(miare::KeyRangeView::prefix(bytes("person:")));
for (bool found = cursor.first(); found; found = cursor.next()) {
    consume(cursor.key(), cursor.value());
}
```

A new cursor is unpositioned. `first()`, `last()`, and `seekLowerBound()` return
whether they found a position. `next()` or `previous()` crosses the range by
returning `false` and making the cursor unpositioned; reposition it before
continuing. Calling `key()` or `value()` without a position throws
`ContractError{Errc::InvalidState}`.

The returned byte views remain valid together until cursor movement,
destruction, transaction termination, or write-cursor invalidation. Every
`put()` and successful `erase()` invalidates all cursors from that write
transaction. Destroy invalidated cursors before committing or rolling back so
debug lifetime checks remain useful.

## Lifetime and threads

Database operations are safe to call concurrently. Transactions, cursors, and
Blob streams are not: functional use must remain on their creating thread and
must not be concurrent. Moving a handle does not transfer that affinity.

Terminal release methods—read `end()`, write `rollback()`, Blob-reader
`close()`, Blob-writer `abort()`, and destruction—may run on another thread if
there is no concurrent operation. Transaction termination invalidates its
cursors and Blob readers. A moved-from or terminal handle is inert, and further
functional use throws `Errc::InvalidState`.

## Commit failures

Before persistence begins, ordinary contract, allocation, provider, capacity,
or I/O failure leaves the write transaction usable. Once commit persistence has
begun, a `DatabaseError` can make the writer terminal and the database
`RecoveryRequired`:

- `Errc::CommitFailed` means the new generation is known not to be published.
- `Errc::CommitOutcomeUnknown` means reopen recovery must determine the result.

Do not retry that transaction state. Close the session, reopen the database,
and inspect an application-level operation identifier if retry detection is
required.

See the [public transaction contract](public-transaction-contract.md) for the
complete lifetime and failure guarantees.
