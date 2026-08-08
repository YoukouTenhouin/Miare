# Design the public C++ API

Type: prototype
Status: resolved

## Question

What is the smallest coherent C++20 API for database creation/opening, byte views and ownership, read and write transactions, point operations, ordered cursors and scans, Blob streaming, commit/rollback, errors, configuration, diagnostics, and clean shutdown—and which lifetime and thread-affinity rules make misuse difficult?

## Resolution

Resolved by the frozen [public and transactional contract](../../../docs/public-transaction-contract.md) and indexed by [implementation ticket 01](../../embedded-kv-v1-implementation/issues/01-freeze-public-and-transactional-contract.md).
