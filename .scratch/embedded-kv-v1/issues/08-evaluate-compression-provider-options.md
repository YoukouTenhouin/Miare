# Evaluate compression provider options

Type: research
Status: resolved

## Question

Which maintained compression formats and provider libraries have suitable licensing, portability, bounded-memory APIs, streaming support, corruption detection, stable format identification, and performance characteristics for ordinary values, B+ tree storage, and chunked Blobs in a desktop embedded database?

## Resolution

Use the RFC 8878 Zstandard frame format with Meta's BSD-licensed reference `libzstd` as the sole v1 compression provider, vendored by default with an optional compatible system-library build. Its stable one-shot and streaming APIs, explicit output and decode-window bounds, portable independently decodable frames, optional corruption checksum, and adjustable speed/density profile cover ordinary values, backend-selected compression units, and independently framed Blob chunks.

Persist database-owned codec/profile identifiers and strict resource limits rather than provider versions. Keep LZ4 Frame/`liblz4` as the first benchmark-driven future alternative, but do not ship a second codec in v1. Defer Brotli and zlib/DEFLATE because they offer no stronger mixed-workload tradeoff for this project. Dictionaries remain excluded unless their persistent lifecycle is specified.

Later specification work completed the initially deferred format choices. [ADR 0036](../../../docs/adr/0036-profile-the-blob-chunk-size.md) makes Blob chunk size part of the capacity profile with a 1 MiB default. [ADR 0038](../../../docs/adr/0038-use-one-bounded-zstandard-frame-per-unit.md) fixes Zstandard profile 1 at level 3, a 16 MiB window bound, no dictionary, no checksum or worker threads, and retention only when the complete extent saves at least one Allocation quantum. [ADR 0064](../../../docs/adr/0064-set-default-profile-performance-release-floors.md) freezes the default-profile release floors against which that policy is qualified. The research asset remains the provider-selection basis rather than the final format policy.

Research asset: [Compression provider options for the embedded KV database](../research/compression-provider-options.md)
