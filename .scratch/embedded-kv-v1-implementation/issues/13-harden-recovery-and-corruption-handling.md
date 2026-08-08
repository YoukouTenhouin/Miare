# 13 — Harden recovery and fail-closed corruption handling

**What to build:** Ensure every specified interrupted or damaged persistent state either recovers the correct committed generation or fails with the correct bounded error without exposing unauthenticated or partially trusted content.

**Blocked by:** 07 — Grow the ordered keyspace beyond a single page; 10 — Compress backend-owned storage transparently; 12 — Complete Blob lifecycle and reclamation.

**Status:** ready-for-agent

- [ ] Deterministic crash-state enumeration covers every persistence boundary for tree, overflow, Blob, allocation, recovery-log, and publication metadata writes.
- [ ] Short and torn writes, permitted reordering, interrupted barriers, and truncated sidecars expose only the last provably durable generation.
- [ ] Mutated, substituted, duplicated, reordered, and malformed authenticated units never release plaintext or partially trusted logical state.
- [ ] Open distinguishes incomplete commit, corruption, authentication failure, wrong key, unsupported format, and provider failure to the extent promised by the frozen format.
- [ ] Usage-limit exhaustion, nonce or randomness failure, allocation failure, and provider failure leave committed data recoverable and do not trigger a security downgrade.
