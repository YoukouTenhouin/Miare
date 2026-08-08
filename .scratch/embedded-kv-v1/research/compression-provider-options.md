# Compression provider options for the embedded KV database

Research date: 2026-08-08

## Recommendation

Adopt the Zstandard frame format and Meta's reference `libzstd` as the sole v1 compression provider. Vendor a reviewed upstream release by default, while allowing the build to link an ABI-compatible system provider. Give the persisted format a library-owned codec identifier for `none` and for the exact supported Zstandard profile; do not derive database compatibility from a provider version number.

Keep LZ4 Frame and upstream `liblz4` as the first qualified alternative if project-specific benchmarks later show that Zstandard consumes too much CPU or memory on hot small-value paths. Do not ship it in v1 merely to preserve optionality: a provider seam and reserved codec-ID space provide that optionality without doubling the conformance and corruption-test matrix.

Do not select Brotli or zlib/DEFLATE for v1. Both are portable and permissively licensed, but neither presents a better general-purpose tradeoff for this database: Brotli is oriented toward stronger density at higher resource cost and its raw format does not provide an integrity checksum, while zlib is mature but materially behind the selected candidates in the upstream Zstandard project's comparative benchmark. These are implementation-selection conclusions, not statements that their formats are unsuitable in general.

## Decision criteria

### Zstandard / `libzstd`

- The Zstandard format is independently specified by [RFC 8878](https://www.rfc-editor.org/rfc/rfc8878.html). It is byte-defined, independent of CPU and operating system, supports arbitrarily long sequential streams with bounded intermediate storage, gives each normal frame a stable magic number, and makes frames independently decodable. Those properties fit a portable persistent database format and chunked Blob storage.
- A frame records its required decode window. The RFC explicitly allows a decoder to reject unreasonable windows and recommends an 8 MiB interoperability ceiling. The reference API additionally exposes `ZSTD_d_windowLogMax`, memory-estimation functions, caller-provided static workspaces, one-shot destination bounds, and streaming contexts in the [official `libzstd` manual](https://facebook.github.io/zstd/zstd_manual.html). The database can therefore impose deterministic per-operation resource limits rather than trusting sizes in a file.
- The stable API covers both ordinary values and Blobs: `ZSTD_compressBound()` bounds one-shot output; `ZSTD_compressStream2()` and `ZSTD_decompressStream()` incrementally consume caller buffers; contexts can be reset and reused. The database should use only upstream's stable API, not the static-link-only experimental API.
- Zstandard frames can carry content size, dictionary ID, and an optional 32-bit content checksum derived from XXH64. The checksum is corruption detection, not authentication; the database's authenticated-encryption and storage-integrity layers remain authoritative. The exact checksum policy belongs to the compression-envelope decision.
- The reference implementation is portable C. Its upstream repository provides a BSD license for the software and describes the project as dual BSD/GPLv2; the permissive BSD option is compatible with vendoring in a desktop library ([upstream repository and license](https://github.com/facebook/zstd), [BSD license text](https://github.com/facebook/zstd/blob/dev/LICENSE)). Upstream releases and build materials explicitly cover Visual Studio/Windows and macOS alongside Unix-like builds, and upstream supplies a supported single-compilation-unit amalgamation path ([release history](https://github.com/facebook/zstd/releases)). This makes it practical even though the database's own code, rather than every provider, is what must be header-only.
- Performance is adjustable across fine-grained compression levels. On the upstream project's current Silesia/lzbench comparison, Zstandard level 1 provides much better density than LZ4 while retaining high throughput, and fast modes trade density for more speed; decompression remains fast across levels ([upstream benchmark and methodology](https://github.com/facebook/zstd#benchmarks)). Those figures are directional only: upstream warns that results depend strongly on content, so the database must benchmark its own value-size and Blob-chunk distributions before freezing the profile ([official benchmark documentation](https://github.com/facebook/zstd/blob/dev/programs/zstd.1.md#benchmark)).
- Dictionary compression could improve small-record density, and the frame has a dictionary identifier, but dictionary lifecycle and long-term availability become persistent-format obligations. Exclude dictionaries from the initial profile unless a later decision explicitly specifies immutable dictionary storage and versioning.

### LZ4 Frame / `liblz4`

- The upstream [LZ4 Frame specification](https://github.com/lz4/lz4/blob/dev/doc/lz4_Frame_format.md) is CPU/OS independent, streamable with bounded intermediate storage, and begins with a stable magic number. It supports independently decodable frames, bounded block sizes from 64 KiB through 4 MiB, optional compressed-block checksums, an optional decoded-content checksum, and an optional content size.
- Upstream `liblz4` has context-based incremental compression and decompression, caller-buffer operation, and `LZ4F_compressBound()` for worst-case output sizing ([official frame API](https://github.com/lz4/lz4/blob/dev/lib/lz4frame.h)). Its 64 KiB history and explicit maximum block size make decode memory easy to cap.
- The library is BSD-2-Clause licensed; upstream distinguishes the permissive library from the GPL-licensed command-line program ([repository](https://github.com/lz4/lz4), [release notes](https://github.com/lz4/lz4/releases)). The current release materials exercise Windows, macOS, and Linux and describe Visual Studio support.
- The upstream Zstandard benchmark shows LZ4 substantially ahead in decompression throughput and somewhat ahead in compression throughput, but with lower density. That makes LZ4 a plausible later choice for extremely hot small values or page-local payloads, provided database-specific tests reproduce a meaningful end-to-end advantage. It is not a reason to carry a second persisted format in v1.

### Brotli and zlib/DEFLATE

- Brotli has an interoperable, bounded-stream format specified by [RFC 7932](https://www.rfc-editor.org/rfc/rfc7932.html), and Google's reference implementation is MIT licensed ([upstream repository](https://github.com/google/brotli)). It can provide strong density, but the reference project explicitly notes that modifications to raw regions may go undetected, and the format has no general content checksum. Database authentication/checksums could compensate, but Brotli adds no compelling v1 advantage over Zstandard for mixed small values and large binary chunks.
- zlib is portable, mature, streaming, and permissively licensed ([upstream source and license](https://github.com/madler/zlib)). Its wrapped format has an Adler-32 integrity check. The upstream Zstandard comparison reports both lower compression and decompression throughput than the selected Zstandard profile on its corpus; backward ecosystem compatibility with `.gz`/zlib streams is not a database requirement, so that maturity does not outweigh the general-purpose tradeoff here.

## Persisted-format and provider constraints implied by the research

These constraints should feed the compression-policy/provider-contract ticket:

1. Persist a database-owned codec/profile ID, uncompressed length, compressed length, and independently authenticated envelope metadata. Never infer the codec from bytes alone, and never store a library version as the codec identity.
2. Define the Zstandard profile narrowly enough for every supported decoder to bound resources: cap window size, compression level, frame size, decoded size, and total output before allocating. Reject unsupported parameters and size mismatches as corruption.
3. Use independent frames at the backend's chosen compression unit. Ordinary values can use one-shot frames; each Blob chunk must be independently framed so seeking, partial reads, recovery, and bounded memory do not depend on decoding the entire Blob. A later LSM backend can frame its own independently recoverable run blocks rather than one monolithic run stream.
4. Compress first and encrypt/authenticate second. Codec checksums are optional defense-in-depth and diagnostics, not a substitute for AEAD or database structural checksums.
5. Store uncompressed bytes when compression fails, exceeds resource limits, or does not clear a specified minimum saving. This prevents expansion and makes provider failure handling deterministic.
6. Provider contexts are transaction-local or thread-local and reusable, never shared concurrently. All provider errors map into stable database error categories without exposing provider enums in the public API.
7. Vendored and system-linked builds must run the same golden-vector, truncation, malformed-frame, oversized-window, output-limit, and cross-provider compatibility tests. Opening a compressed database must fail with `unsupported_format` if the required codec/profile was compiled out; it must never silently treat compressed bytes as raw.

## Remaining empirical work

The format/provider choice is supportable from primary-source facts, but default tuning is not. Before freezing compression level, Blob chunk size, minimum-savings threshold, context-pool limits, or whether codec checksums are enabled, benchmark representative desktop data on x86-64 and ARM64 Windows/macOS/Linux. Measure tiny values separately from page-sized values and incompressible/already-compressed Blob chunks; include latency percentiles, peak memory, write amplification, and total database size. This is a verification task, not a reason to keep the v1 provider undecided.
