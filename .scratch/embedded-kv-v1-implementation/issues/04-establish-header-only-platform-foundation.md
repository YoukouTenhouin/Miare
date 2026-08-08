# 04 — Establish the header-only platform and provider foundation

**What to build:** Provide a consumable C++20 project foundation whose own database implementation is header-only and whose narrow platform, cryptography, and compression seams can be exercised deterministically on every supported operating system.

**Blocked by:** 01 — Freeze the public and transactional contract.

**Status:** ready-for-agent

- [ ] A minimal consumer builds and links on the supported Windows, Linux, and macOS toolchains without a separately built database library.
- [ ] Public byte, ownership, result, and stable error primitives match the frozen contract and cover expected failures without exceptions.
- [ ] Exact positioned I/O, resize, stable-storage barriers, and defensive single-process locking are exposed through a portable durable-file seam.
- [ ] AES-256-GCM-SIV, HKDF-SHA-256, secure randomness, and Zstandard provider adapters enforce input, output, memory, and authentication bounds.
- [ ] Deterministic provider and durable-file substitutes can inject short I/O, barrier failures, corruption, randomness failures, and provider errors in tests.
