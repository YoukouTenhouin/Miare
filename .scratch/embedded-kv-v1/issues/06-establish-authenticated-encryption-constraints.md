# Establish authenticated-encryption constraints

Type: research
Status: resolved

## Question

Which current primary cryptographic standards and provider documentation define suitable authenticated-encryption constructions, nonce-misuse constraints, tag and key sizes, domain separation, random-access chunking, and safe limits for an offline-threat embedded database, including its journal/WAL and Blob data?

## Resolution

Adopt RFC 8452 AEAD_AES_256_GCM_SIV as the only v1 encrypted-format suite: 32-byte derived key, 12-byte fresh random nonce, and untruncated 16-byte tag. Derive purpose-separated header, main-data, WAL/journal, and Blob keys with HKDF-SHA-256 and canonical context labels. Encrypt bounded pages/records, log frames, and Blob chunks independently; canonical AAD plus authenticated roots/manifests must bind database identity, domain, type, location/logical identity, generation/sequence, lengths, compression facts, and chunk/frame order. Never release unauthenticated plaintext, enforce RFC usage limits, fail rather than downgrade when the suite is unavailable, and explicitly exclude whole-file rollback detection without trusted external state.

Research asset: [Authenticated-encryption constraints for the embedded KV database](../research/authenticated-encryption-constraints.md)
