# Set the capacity and performance envelope

Type: grilling
Status: resolved

## Question

Which representative desktop workloads, database sizes, key/value distributions, Blob sizes, transaction sizes, reader counts, memory budgets, latency percentiles, throughput expectations, and file-growth limits must guide the architecture and become explicit v1 acceptance targets?

## Resolution

Resolved by the frozen [public and transactional contract](../../../docs/public-transaction-contract.md) and [recovery, maintenance, and verification contract](../../../docs/recovery-maintenance-verification-contract.md), indexed by implementation tickets [01](../../embedded-kv-v1-implementation/issues/01-freeze-public-and-transactional-contract.md) and [03](../../embedded-kv-v1-implementation/issues/03-freeze-recovery-maintenance-and-verification.md).

The public contract fixes the compile-time capacity profile, runtime cache and transaction budgets, and representative workload of up to 1 TiB committed data, 100 million keys, 256 readers, typical 16–256 byte keys, 100 byte–64 KiB Values, 1 MiB–10 GiB Blobs, and 1–10,000-mutation writes. [ADR 0064](../../../docs/adr/0064-set-default-profile-performance-release-floors.md) and the qualification contract fix six-target default-profile floors for latency, throughput, memory overhead, file amplification, checkpoint, verification, backup, and compaction, plus a 15 percent baseline-regression ceiling. Absolute profile limits remain correctness and interoperability bounds, not performance promises.
