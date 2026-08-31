# Transactional Blobs

Values are convenient owned byte strings bounded by `maxValueBytes`. Blobs are
separate database objects for content that should be written and read
incrementally, potentially far beyond that limit.

Miare does not infer relationships between values and Blobs. Applications
normally serialize a `BlobId` into a value and maintain referential integrity
inside the same write transaction.

## Create a Blob

```cpp
auto write = database.beginWrite();
auto blob = write.createBlob();
const miare::BlobId id = blob.id();

blob.write(firstChunk);
blob.write(secondChunk);
blob.finish();

const auto encodedId = id.toBytes();
write.put(bytes("featured-asset"), encodedId);
write.commit();
```

Writers begin at offset zero and are sequential. `position()` reports the
logical bytes accepted. Empty writes and empty Blobs are valid.

`finish()` makes the new content visible within the write transaction but does
not commit it. Every Blob writer must be finished or aborted before commit;
otherwise `commit()` throws `Errc::InvalidState` and leaves the transaction
active. Destroying or calling `abort()` on an unfinished writer cancels only
that Blob mutation.

## Read a Blob

Deserialize the application-owned identifier and open it through a transaction:

```cpp
auto stored = read.get(bytes("featured-asset"));
if (!stored || stored->size() != miare::BlobId::encodedSize) {
    handleMissingOrInvalidReference();
    return;
}
auto id = miare::BlobId::fromBytes(
    std::span<const std::byte, miare::BlobId::encodedSize>{
        stored->data(), stored->size()});

auto opened = read.openBlob(id);
if (opened) {
    auto blob = std::move(*opened);
    std::array<std::byte, 64 * 1024> buffer;
    while (const std::size_t count = blob.read(buffer)) {
        consume(miare::ByteView{buffer}.first(count));
    }
    blob.close();
}
```

`read()` advances the current position and returns zero at end-of-Blob. `seek()`
accepts an absolute offset from zero through `size()`. Blob readers retain the
content version they opened even when a writer replaces that identity.

Close readers before ending their transaction. The complete build-tested
version is [`examples/blob.cpp`](https://github.com/YoukouTenhouin/Miare/blob/master/examples/blob.cpp).

## Replace or erase

`replaceBlob(id)` returns an optional writer. Absence means that identifier does
not exist in the write transaction's view. The old version remains visible
until the replacement writer finishes; after that, new readers in the writer
see the replacement while already-open readers retain their version.

`eraseBlob(id)` returns whether a Blob existed. It rejects an identifier with an
unfinished writer. Replacement or erasure becomes durable atomically with all
key mutations at commit.

## Identifier properties

`BlobId` is an opaque, random 128-bit identity local to a database. It has a
canonical 16-byte encoding, equality and ordering, and `std::hash` support. It
does not contain database identity, time, generation, or global uniqueness.
Using an identifier from another database simply observes absence.

V1 has no append-specific operation, sparse content, truncation, writable seek,
partial update, or database-managed reference tracking. Replace the complete
content when it changes.
