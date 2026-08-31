# V1 qualification

V1 release qualification is executable but intentionally split between fast
pull-request checks and scheduled/manual long-running gates. A green ordinary
CI run is not, by itself, a claim that the duration and reference-hardware
requirements in the frozen maintenance contract have been met.

## Backend conformance

Configure the opt-in qualification tests and run the labeled suite:

```sh
cmake -S . -B build-qualification \
  -DMIARE_BUILD_TESTS=ON \
  -DMIARE_BUILD_QUALIFICATION_TESTS=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-qualification --parallel
ctest --test-dir build-qualification -L qualification --output-on-failure
```

The ordered-keyspace model executes 1,000 fixed seeds of 10,000 operations.
The Blob model uses the same seed and operation counts. The concurrency gate
uses 256 readers and 10,000 writer iterations. All compare observable public
results and handle state; they do not assert allocation order or page layout.

## Six-target fixture interchange

`.github/workflows/qualification.yml` builds on Linux, Windows, and macOS for
x86-64 and ARM64. Every target creates the complete encryption
`None`/`XChaCha20Poly1305Ietf` by compression `None`/`ZStd` fixture matrix plus
unsupported-feature fixtures. A second six-target matrix downloads the whole
corpus and verifies, opens, reads, mutates, cleanly closes, backs up, and reopens
every producer's valid files. It also checks keyed/keyless API mismatch,
wrong-key authentication rejection, suite-0 corruption checksums, suite-1 byte
compatibility, and unsupported-feature classification. Workflow artifacts are
retained for 30 days; promoted release corpora belong in version control.

Installed-package qualification additionally configures consumers with no
optional provider dependencies, libsodium only, Zstandard only, and both. Each
consumer exercises exactly the modes its capabilities support and proves that
disabled capabilities are neither included, linked, initialized, nor invoked.

## Fuzzing and sanitizers

Build the bounded libFuzzer targets with Clang:

```sh
cmake -S . -B build-fuzz \
  -DMIARE_BUILD_TESTS=OFF \
  -DMIARE_BUILD_FUZZERS=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_COMPILER=clang++
cmake --build build-fuzz --parallel
build-fuzz/fuzz/miare_format_fuzz fuzz/corpus/format -max_len=16777216
build-fuzz/fuzz/miare_public_history_fuzz \
  fuzz/corpus/public_history -max_len=65536
```

The format target bounds input at 16 MiB and exercises publication selection,
root parsing, authentication, decompression, allocator validation, and full
verification. The public-history target bounds input at 64 KiB and 1,024
operations while exercising transactions, reads, Blobs, diagnostics,
checkpointing, and verification. Pull requests run 10,000 sanitizer-backed
iterations per target. Release evidence still requires the frozen 24/72
CPU-hour campaigns and retention of every minimized discovery.

## Benchmarks

Build, run, and validate the machine-readable report:

```sh
cmake -S . -B build-benchmark \
  -DMIARE_BUILD_TESTS=OFF \
  -DMIARE_BUILD_BENCHMARKS=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-benchmark --parallel
build-benchmark/benchmarks/miare_qualification_benchmark \
  --output miare-qualification-benchmark.json
python3 benchmarks/validate.py miare-qualification-benchmark.json
```

On Windows, invoke the validator with `python` instead of `python3`.

The report includes cache-hit lookup and 100-mutation transaction percentiles,
ordered-scan and Blob throughput, verification and physical-backup throughput,
file amplification, reclamation before/after checkpoint, no-op checkpoint p99,
and clean-close cost. Scheduled CI retains the JSON as trend evidence. Formal
release sign-off runs five trials after one warm-up on each published reference
desktop and additionally records peak database-owned memory and regression
deltas against the previous accepted baseline.
