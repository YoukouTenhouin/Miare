# 04 — Establish the header-only platform and provider foundation

**What to build:** Provide a consumable C++20 project foundation whose own database implementation is header-only and whose narrow platform, cryptography, and compression seams can be exercised deterministically on every supported operating system.

**Blocked by:** 01 — Freeze the public and transactional contract.

**Status:** resolved

- [x] A minimal consumer builds and links on the supported Windows, Linux, and macOS toolchains without a separately built database library.
- [x] Public byte, ownership, result, and stable error primitives match the frozen contract and cover expected failures without exceptions.
- [x] Exact positioned I/O, resize, stable-storage barriers, and defensive single-process locking are exposed through a portable durable-file seam.
- [x] XChaCha20-Poly1305-IETF, keyed BLAKE2b-256 and the BLAKE2b KDF, secure randomness, and Zstandard provider adapters enforce input, output, memory, nonce, derivation, and authentication bounds.
- [x] Deterministic provider and durable-file substitutes can inject short I/O, barrier failures, corruption, randomness failures, and provider errors in tests.

## Resolution

The project now exports the header-only `miare::miare` CMake target and the
single documented `<miare/database.hpp>` entry point. The public foundation
implements the frozen byte views, allocator-scoped owned bytes, Blob identity,
capacity defaults, semantic `Result<T, E>`, stable error families, and the
move-only database ownership shape. An installed-package fixture verifies that
a separate minimal consumer can discover and link the target without a built
Miare library.

The platform layer provides exact positioned I/O, resize, platform-specific
stable-storage barriers, and defensive exclusive locking for Windows, Linux,
and macOS. The provider layer uses libsodium-compatible XChaCha20-Poly1305-IETF
and BLAKE2b derivation plus bounded Zstandard profile 1. Fixed cryptographic
vectors, strict authentication and codec validation, and compatibility with
older Zstandard headers are covered by the provider tests.

Reusable test substitutes provide deterministic randomness and provider
failures, ciphertext and frame corruption, short and failed I/O, resize and
barrier failures, operation logging, and stable-versus-unbarriered crash-state
selection including partial retention, reordered writes, and retained resize.
CI exercises the Linux build in an official openSUSE Tumbleweed container and
retains macOS and Windows jobs. The implementation and review corrections are
recorded in commits `b0a709b` through `970075e`; follow-up CI compatibility is
recorded in `bd82752` and `d98a407`.
