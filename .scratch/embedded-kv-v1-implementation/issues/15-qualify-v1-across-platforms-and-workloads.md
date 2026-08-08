# 15 — Qualify v1 across platforms and workloads

**What to build:** Demonstrate that the complete v1 backend meets its public contract, portability, security, reliability, capacity, and performance promises on Windows, Linux, and macOS.

**Blocked by:** 08 — Traverse ordered, prefix, and bounded ranges; 09 — Support concurrent snapshots and serialized writers; 12 — Complete Blob lifecycle and reclamation; 14 — Deliver maintenance and portable backup operations.

**Status:** ready-for-agent

- [ ] The backend-conformance suite covers lifecycle, exact operations, scans, transactions, contention, snapshots, Blobs, diagnostics, failures, maintenance, and shutdown solely through observable contracts.
- [ ] Compatibility fixtures are created and consumed across all supported operating systems for encrypted, compressed, cleanly closed, supported-version, unsupported-feature, and wrong-key cases.
- [ ] Public histories, file parsing, recovery records, envelopes, compression frames, cursors, ranges, and Blob manifests pass bounded fuzzing and sanitizer gates.
- [ ] Long-duration concurrency and deterministic fault campaigns meet the frozen recovery, corruption, and resource-pressure criteria.
- [ ] Representative desktop benchmarks report latency percentiles, throughput, peak memory, file amplification, reclamation, maintenance, and clean-close costs within the frozen envelope.
- [ ] Benchmark-derived compression defaults and other format-affecting thresholds are validated consistently across supported platforms before v1 is declared ready.
