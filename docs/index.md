# Miare V1 documentation

Miare is a header-only C++20 embedded database with an ordered byte-string
keyspace and first-class transactional Blobs. Every committed database is
authenticated and encrypted. The default backend uses copy-on-write B+ tree
generations, snapshot readers, and synchronous durable commits.

Start with these guides:

1. [Getting started](getting-started.md) installs Miare and creates a database.
2. [Transactions and ordered scans](transactions.md) covers keys, values,
   snapshots, cursors, and concurrency.
3. [Transactional Blobs](blobs.md) covers streaming large content.
4. [Operations, errors, and recovery](operations.md) covers deployment and
   operational behavior.
5. The generated site's **Data Structures** section lists every documented
   public C++ type and member.

The guides explain normal application use. The
[public transaction contract](public-transaction-contract.md),
[recovery and maintenance contract](recovery-maintenance-verification-contract.md),
and [portable format](portable-btree-blob-format.md) are the normative sources
for exact V1 behavior and compatibility.

## What Miare stores

The ordered keyspace maps arbitrary byte-string keys to arbitrary byte-string
values. Keys are ordered lexicographically by unsigned byte value. Miare does
not know whether bytes represent text, integers, structured records, or encoded
Blob identifiers; serialization and relationships belong to the application.

A Blob is a separate database object for content that should be read or written
incrementally. A stable 128-bit `BlobId` identifies it within one database.
Replacing a Blob changes its content atomically without changing that identity.

## Qualified V1

V1 has frozen public, transactional, recovery, and portable-format contracts.
Its qualification suite covers supported compilers and operating systems,
cross-target file interchange, bounded fuzzing, sanitizer gates, concurrency,
and the default-profile performance floors. See
[V1 qualification](v1-qualification.md) for the release gates.
