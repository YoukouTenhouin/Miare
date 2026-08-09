# Derive database keys with libsodium-compatible BLAKE2b

V1 replaces the inherited HKDF-SHA-256 assumption with a two-stage libsodium-compatible BLAKE2b construction. Keyed BLAKE2b-256 first derives a database-specific 32-byte root from the caller's 32-byte high-entropy key, a random persisted 16-byte salt, canonical database and suite identity, derivation version, and fixed 16-byte personalization; `crypto_kdf_derive_from_key()` then uses the fixed 8-byte context `MiareV1K` and stable subkey IDs to derive separate 32-byte header, main-data, recovery-data, and Blob keys. This aligns the KDF with the chosen provider ecosystem while preserving per-database and per-domain separation; every byte of the derivation and cross-provider fixtures is part of the portable format.

The construction uses libsodium's documented [BLAKE2b key-derivation API](https://doc.libsodium.org/key_derivation) and its lower-level [BLAKE2b salt and personalization parameters](https://doc.libsodium.org/hashing/generic_hashing).

The first-stage message is exactly the 16-byte database identity followed by the encryption-suite identifier and derivation version as little-endian `u32` values, with no length prefix or other bootstrap or profile bytes. The existing salt, personalization, context, and subkey identifiers complete the byte-exact derivation.
