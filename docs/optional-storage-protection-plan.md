# Optional storage protection delivery plan

Status: accepted V1 correction

Miare V1 treats encryption and compression as independent creation-time
capabilities. The supported matrix is:

| Encryption | Compression | Crypto provider | Compression provider |
|---|---|---|---|
| None | None | not used | not used |
| None | Zstandard | not used | required |
| XChaCha20-Poly1305-IETF | None | required | not used |
| XChaCha20-Poly1305-IETF | Zstandard | required | required |

This corrects the original encrypted-only V1 contract. It does not create a
V2 format and does not permit changing either choice after creation. Existing
suite-1 files and every byte used to encode them remain unchanged.

## Investigation findings

The original implementation conflates protection with authenticated
encryption in six places:

1. `CreateOptions` and the public `create`, `open`, and `verifyFile` signatures
   always require a key and expose only the XChaCha suite.
2. `ProviderSet::system()` eagerly initializes cryptography and compression,
   and its representation requires a crypto provider even for
   `Compression::None`.
3. Database identity, KDF salt, publication nonces, extent nonces, capacity
   digests, and Blob identifiers all enter through the crypto-provider seam.
4. Publication selection assumes that a usable slot is one whose AEAD tag
   verifies and that failure of both slots is always `AuthenticationFailed`.
5. Every backend extent is sealed and opened through AEAD, and its fixed
   geometry is described exclusively as nonce, ciphertext, and tag.
6. Recovery, verification, backup, checkpoint, compaction, and qualification
   use “authenticated” as both a framing property and a security guarantee.

Compression dispatch is already conditional for backend payloads: a
`Compression::None` file neither compresses nor decompresses an extent. The
remaining coupling is construction, header inclusion, package discovery, and
linking of the provider rather than a data-path requirement.

The complete operational trace is:

- create builds the bootstrap and two publication slots, installs the file,
  then reopens it through the normal selection and shallow-root path;
- open and offline verification parse the bootstrap, select one publication,
  and shallowly validate roots before deeper traversal;
- transactions, Blob streaming, allocator persistence, checkpoint, and
  compaction all converge on the same extent preparation and publication
  functions;
- ordinary reads, verification, backup, recovery, and compaction all converge
  on the same protected-extent decoder;
- backup copies the already verified committed prefix without transforming its
  protection or compression representation; and
- package consumers receive both provider headers and libraries transitively
  from the single header-only CMake target.

These shared paths make one protection-policy dispatch for publication slots
and one for extents preferable to parallel encrypted and unencrypted storage
engines.

## Corrected protection model

The umbrella term is **Protected unit**: a self-framing publication slot or
backend extent whose structural context and payload are checked together
before decoding. An encrypted protected unit is an **Authenticated unit** and
has the existing confidentiality and adversarial-integrity guarantees. An
unencrypted protected unit is a **Checksummed unit** and provides deterministic
accidental-corruption detection plus canonical structural validation only.

Unencrypted files use no caller key, KDF salt, derived key, nonce generation,
AEAD encryption, or AEAD decryption. Their application keys, Values, Blob
bytes, and allocator bytes are plaintext. Their unkeyed checksums are publicly
recomputable and therefore provide no cryptographic authentication, tamper
resistance, confidentiality, or origin claim.

Both modes retain the common-region and extent geometry so recovery,
allocation, backup, and maintenance continue to operate over one format. The
portable identifiers and exact encoding are frozen in
`portable-btree-blob-format.md`.

## Public API and failures

The existing keyed APIs remain the sole encrypted entry points and continue to
accept exactly 32 bytes for suite 1. Keyless operations use separately named
`createUnencrypted`, `openUnencrypted`, and `verifyUnencryptedFile` functions;
there is no empty-key convention and no nullable key parameter.

`UnencryptedCreateOptions` contains storage-backend and compression choices
but no encryption-suite or key field. Its default compression is `None`, so
the shortest keyless creation path needs neither optional provider. Existing
`CreateOptions` remains the keyed creation type and continues to default to
the byte-compatible suite-1/Zstandard configuration.

The opening entry point must agree with the visible suite identifier:

- a keyed operation on suite 0 throws `DatabaseError{Errc::UnexpectedKey}`;
- a keyless operation on suite 1 throws `DatabaseError{Errc::KeyRequired}`;
- a suite-1 key with the wrong size throws
  `ContractError{Errc::InvalidArgument}`;
- a correctly sized wrong suite-1 key returns `AuthenticationFailed`;
- a required but unavailable provider throws
  `DatabaseError{Errc::ProviderUnavailable}`; and
- unknown suite, KDF, or codec identities retain the documented compatibility
  classifications and never cause downgrade or fallback. Suite identifier 0
  fixes the BLAKE2b-128 corruption checksum directly; it has no independently
  variable checksum identifier.

`UnexpectedKey` and `KeyRequired` are appended stable error values. They
reflect the bounded visible bootstrap dispatch and reveal no protected
content.

## Provider and build decision

Provider ownership becomes capability-based. A provider set may contain
crypto, compression, both, or neither; requiring a capability happens only
after the selected suite or codec proves it is needed. System constructors
allow applications to request the compiled crypto and compression
capabilities independently.

Database and Blob identity entropy is a format-neutral internal capability,
not an encryption operation. It is available without libsodium and remains
injectable for deterministic tests. Unencrypted files persist a zero KDF salt
and zero nonces; Blob identifiers and the database identity still use the
format-neutral entropy source and are collision-checked as before.

Completely optional libsodium and Zstandard dependencies are coherent and are
part of this series. `MIARE_ENABLE_SODIUM` and `MIARE_ENABLE_ZSTD` are
independent CMake options that default on to preserve the full existing build.
The installed target publishes `MIARE_HAS_SODIUM` and `MIARE_HAS_ZSTD`
capability definitions. Provider headers are included only when their
capability is enabled. `ProviderSet::none()` is always available;
`systemCrypto()` and `systemCompression()` are available with their respective
capabilities, and the existing `system()` convenience remains when both are
enabled. Consumers using suite 0 with `Compression::None` will compile, link,
create, reopen, verify, transact, maintain, and back up without either
dependency.

## Ordered pull requests

1. **Define the corrected V1 contract and protection model**
   (`optional-storage-protection`): update terminology, ADRs, public and
   portable contracts, failure semantics, provider/build decisions, and
   encrypted byte-characterization tests. Add internal seams only when they do
   not expose a half-functional mode.
2. **Implement genuine unencrypted databases**
   (`implement-unencrypted-databases`): add the keyless API and suite-0
   publication/extent paths across transactions, Blobs, recovery,
   verification, backup, checkpoint, and compaction. Make runtime provider
   ownership capability-based so the keyless/no-compression path owns neither
   optional provider. Prove zero crypto-provider operations and retain suite-1
   bytes.
3. **Decouple optional providers and dependencies**
   (`decouple-optional-providers`): make the system-provider headers,
   constructors, libsodium, and Zstandard independently configurable
   CMake/package capabilities, and add installed consumers for all dependency
   combinations.
4. **Qualify and document the complete mode matrix**
   (`qualify-optional-storage-modes`): add portable fixtures, cross-read,
   corruption, recovery, maintenance, backup, provider, sanitizer, package,
   and six-target qualification for all four modes; update user-facing
   documentation and security claims.

Each pull request follows the repository dev/reflow workflow, starts from the
merged predecessor, passes its complete relevant gates, completes the Copilot
review and independent comment-triage loop, and merges before the next begins.
Final completion requires the complete suite on updated `master`.
