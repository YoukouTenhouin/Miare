# Establish authenticated-encryption constraints

Type: research
Status: superseded

## Question

Which current primary cryptographic standards and provider documentation define suitable authenticated-encryption constructions, nonce-misuse constraints, tag and key sizes, domain separation, random-access chunking, and safe limits for an offline-threat embedded database, including its journal/WAL and Blob data?

## Superseded resolution

Adopt RFC 8452 AEAD_AES_256_GCM_SIV as the only v1 encrypted-format suite: 32-byte derived key, 12-byte fresh random nonce, and untruncated 16-byte tag. Derive purpose-separated header, main-data, WAL/journal, and Blob keys with HKDF-SHA-256 and canonical context labels. Encrypt bounded pages/records, log frames, and Blob chunks independently; canonical AAD plus authenticated roots/manifests must bind database identity, domain, type, location/logical identity, generation/sequence, lengths, compression facts, and chunk/frame order. Never release unauthenticated plaintext, enforce RFC usage limits, fail rather than downgrade when the suite is unavailable, and explicitly exclude whole-file rollback detection without trusted external state.

These agent-selected AES-256-GCM-SIV and HKDF-SHA-256 assumptions were superseded by the explicit product decisions in [ADR 0015](../../../docs/adr/0015-use-xchacha20-poly1305-ietf.md) and [ADR 0018](../../../docs/adr/0018-derive-database-keys-with-blake2b.md): v1 uses libsodium-compatible XChaCha20-Poly1305-IETF and a salted, two-stage BLAKE2b database-key derivation. The linked research remains historical analysis; its bounded-unit, authenticated-manifest, fail-closed, and threat-boundary conclusions still apply where construction-independent.

Research asset: [Authenticated-encryption constraints for the embedded KV database](../research/authenticated-encryption-constraints.md)
