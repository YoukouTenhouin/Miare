# Evaluate compression provider options

Type: research
Status: resolved

## Question

Which maintained compression formats and provider libraries have suitable licensing, portability, bounded-memory APIs, streaming support, corruption detection, stable format identification, and performance characteristics for ordinary values, B+ tree storage, and chunked Blobs in a desktop embedded database?

## Resolution

Use the RFC 8878 Zstandard frame format with Meta's BSD-licensed reference `libzstd` as the sole v1 compression provider, vendored by default with an optional compatible system-library build. Its stable one-shot and streaming APIs, explicit output and decode-window bounds, portable independently decodable frames, optional corruption checksum, and adjustable speed/density profile cover ordinary values, backend-selected compression units, and independently framed Blob chunks.

Persist database-owned codec/profile identifiers and strict resource limits rather than provider versions. Keep LZ4 Frame/`liblz4` as the first benchmark-driven future alternative, but do not ship a second codec in v1. Defer Brotli and zlib/DEFLATE because they offer no stronger mixed-workload tradeoff for this project. Default compression level, minimum-savings threshold, Blob chunk size, and codec-checksum policy require representative cross-platform benchmarks in later specification tickets; dictionaries are excluded unless their persistent lifecycle is specified.

Research asset: [Compression provider options for the embedded KV database](../research/compression-provider-options.md)
