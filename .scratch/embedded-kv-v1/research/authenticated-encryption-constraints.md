# Authenticated-encryption constraints for the embedded KV database

Researched 2026-08-08. Sources are limited to primary standards and official provider documentation.

## Recommendation

Use **AEAD_AES_256_GCM_SIV** from RFC 8452 as the single v1 encrypted-format suite, with **HKDF-SHA-256** for the per-database key hierarchy. Encrypt every independently addressable storage unit as a separate AEAD message. This means pages/records, WAL or journal frames, and Blob chunks can be authenticated before use and read independently. Record the suite identifier in the file format and fail closed when the configured provider cannot implement it; never substitute another construction silently.

AES-GCM-SIV is a better default for crash-recoverable storage than ordinary AES-GCM or ChaCha20-Poly1305 because accidental nonce repetition is not catastrophic. Repetition still leaks whether plaintexts are equal, so v1 must generate a fresh random nonce for every encryption and must account for use limits. RFC 8452 specifically recommends random nonces and describes the construction as nonce-misuse resistant, while warning that misuse resistance is not permission to intentionally reuse nonces ([RFC 8452, Sections 1 and 9](https://www.rfc-editor.org/rfc/rfc8452)).

This choice does not make a FIPS claim. OpenSSL 3.2 added AES-GCM-SIV to its default provider but not its FIPS provider, so FIPS-only operation would require a different future format suite or a validated provider that supports this exact suite ([OpenSSL 3.2 AES cipher documentation](https://docs.openssl.org/3.2/man7/EVP_CIPHER-AES/)).

## Why not the common alternatives as the v1 format

- NIST's current AES-GCM recommendation requires careful IV construction, and NIST says GCM security depends strongly on preventing IV repetition. NIST is still preparing a revision as of 2026, so the 2007 SP 800-38D remains the published recommendation ([NIST SP 800-38D](https://csrc.nist.gov/pubs/sp/800/38/d/final), [NIST's 2026 revision notice](https://csrc.nist.gov/News/2026/gcm-and-gmac-block-cipher-modes-of-operation)). A database can enforce uniqueness with durable counters, but crash rollback, cloned files, and independent WAL/main-file writers make that state part of the recovery proof. It is unnecessary risk for the baseline.
- IETF ChaCha20-Poly1305 has a 256-bit key, 96-bit nonce, and 128-bit tag, but the nonce must not repeat for the same key; RFC 8439 describes loss of plaintext confidentiality when it does ([RFC 8439, Sections 2.8 and 4](https://www.rfc-editor.org/rfc/rfc8439)).
- Libsodium recommends XChaCha20-Poly1305 when interoperability is not a concern and says its 192-bit nonce makes random nonces safe, but also notes that this variant is not as broadly interoperable as the IETF construction ([libsodium ChaCha20-Poly1305 documentation](https://doc.libsodium.org/secret-key_cryptography/aead/chacha20-poly1305)). It remains a plausible future suite, not a second v1 format option.

## Normative parameters and limits

The encrypted-format contract should fix these values rather than accepting provider-selected variants:

| Parameter | v1 constraint | Basis |
|---|---:|---|
| AEAD | AEAD_AES_256_GCM_SIV | RFC 8452 |
| Derived AEAD key | 32 bytes | RFC 8452 Section 6 |
| Nonce | 12 bytes | RFC 8452 Section 6 |
| Authentication tag | 16 bytes, never truncated | RFC 8452 Sections 5-6 |
| RFC plaintext maximum per invocation | `2^36` bytes | RFC 8452 Section 6 |
| RFC AAD maximum per invocation | `2^36` bytes | RFC 8452 Section 6 |

Those RFC maxima are cryptographic ceilings, not sensible allocation sizes. The physical-format and Blob tickets must impose much smaller bounded units so decryption can withhold the entire plaintext until authentication succeeds. RFC 8452 explicitly requires that unauthenticated plaintext never be released and calls out memory pressure as a design concern ([RFC 8452, Section 9](https://www.rfc-editor.org/rfc/rfc8452)).

The implementation must count encryptions per derived key and refuse writes before a suite use limit is crossed. RFC 8452 gives random-nonce bounds, for an adversarial advantage no greater than `2^-32`, including `2^64` messages of at most 128 KiB, `2^48` messages of at most 32 MiB, or `2^32` messages of at most 8 GiB. The eventual page and Blob chunk maxima must select the applicable conservative bound; v1 must not invent an unbounded `uint64_t` promise ([RFC 8452, Section 9](https://www.rfc-editor.org/rfc/rfc8452)).

## Nonces, keys, and domain separation

1. Require at least 256 bits of caller-supplied high-entropy key material. Password processing remains outside the library.
2. Generate a fresh random per-database salt at creation and use HKDF-Extract/HKDF-Expand with SHA-256 to derive 32-byte internal keys. RFC 5869 defines extract-then-expand and says the `info` input binds derived material to application- and context-specific information ([RFC 5869, Sections 2-3](https://www.rfc-editor.org/rfc/rfc5869)).
3. Use canonical, length-delimited `info` labels containing a fixed project identifier, KDF/envelope version, suite identifier, and purpose. At minimum, derive distinct keys for bootstrap/header authentication, main backend data, WAL/journal data, and Blob data. More granular subkeys are allowed, but the same label must never mean two purposes.
4. Generate a fresh uniform 96-bit nonce from the provider CSPRNG for every AEAD invocation and store it beside the ciphertext. Do not use a fixed nonce as policy, derive a nonce from a mutable file offset alone, or reset a counter after recovery. AES-GCM-SIV supplies a safety net for accidental collision; it does not remove nonce lifecycle requirements.
5. A copied database and its sidecars retain their old valid ciphertext. Fresh random nonces permit the copy to evolve without coordinating a shared counter, although applications must still avoid concurrently treating two copies as one database.

## Authenticated units and associated data

AEAD authenticates only the exact `(key, nonce, ciphertext, AAD)` invocation. The format must therefore define a canonical, endian-stable AAD encoding and use it to bind ciphertext to its meaning. Every unit's AAD should include, as applicable:

- format/envelope version and suite identifier;
- database identifier and storage domain (`header`, `main`, `wal`, `journal`, `blob`);
- backend identifier and record/page/chunk type;
- logical identity plus incarnation/generation or transaction sequence;
- expected plaintext length and all parsing-relevant flags, including compression codec/state;
- Blob identifier, chunk index, and logical length for Blob chunks;
- transaction/frame sequence and record kind for WAL or journal frames.

An authenticated root/manifest must commit to the set and order of live units. Per-unit AEAD alone does not detect deletion of a unit, substitution of an older valid unit, reordering, or truncation of the tail. WAL/journal commit markers and Blob manifests must therefore authenticate counts, ordering, transaction identity, and final state.

Minimal bootstrap fields needed to locate the salt, suite, versions, nonce, ciphertext, and tag may remain visible, but no unauthenticated field may be trusted for allocation, offsets, codec selection, backend dispatch, or recovery decisions beyond tightly checked bounds. Authentication failure must yield no plaintext and one stable authentication/corruption error category; it must not expose whether the key, tag, AAD, or ciphertext was wrong.

## Random-access and streaming implications

- A B+ tree page or encrypted value is one independently authenticated unit.
- Each WAL/journal frame is independently authenticated; an authenticated commit/footer record binds the frame sequence so a valid prefix cannot masquerade as a committed transaction.
- A Blob is not one enormous AEAD message. It is a manifest plus bounded, independently authenticated chunks. A read may release a chunk only after that chunk verifies. The manifest binds Blob identity, total logical length, chunk count/order, compression facts, and content generation so chunks cannot be moved between blobs or versions.
- Provider “streaming” APIs must not define the portable format. The database-level chunk format provides streaming and random access; the provider operation for each unit is logically one-shot/detached AEAD.

## Provider acceptance constraints

A provider is suitable only if it:

- implements the exact RFC 8452 AES-256-GCM-SIV construction and parameters;
- supplies cryptographically secure random bytes for nonces and salts;
- supports HKDF-SHA-256 or allows the library to use a separately vetted HKDF provider;
- withholds plaintext until tag verification and leaves output unusable on failure;
- supports canonical AAD and rejects truncated tags or malformed sizes;
- passes RFC 8452 known-answer, counter-wrap, tamper, wrong-key, wrong-AAD, and boundary tests;
- exposes enough information for the library to enforce message and invocation limits;
- provides secure erasure for provider-owned transient key/plaintext buffers where the platform permits it.

The portable bytes on disk, not the provider API, are authoritative. Cross-provider conformance tests must prove that one provider can decrypt another provider's fixtures on every supported platform.

## Threat-model boundary

The construction protects confidentiality and detects modification, relocation, reordering, and truncation only when the surrounding authenticated metadata binds those properties. It cannot detect whole-database rollback or replacement with an older, internally valid snapshot when the attacker can replace every file and the application keeps no trusted external state. V1 should state that rollback/fork detection is outside its offline-integrity guarantee rather than imply AEAD solves it.
