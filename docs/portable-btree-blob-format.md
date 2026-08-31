# Portable B+ tree and Blob format

Status: v1 frozen, corrected for optional storage protection

This document is the canonical persistent-format specification for Miare's v1 B+ tree Storage backend and transactional Blobs. A conforming implementation must reject representations that violate a stated canonical form even if their logical content could otherwise be interpreted.

## Format architecture

The backend publishes immutable copy-on-write generations. Each committed generation identifies one protected root set covering the ordered keyspace, Blob catalog, and allocation state. A transaction writes replacement structures without modifying any Protected extent reachable from a committed generation, then publishes its root set atomically. Superseded extents remain available while a live snapshot can observe their generation and become reclaimable afterward.

The v1 B+ tree backend stores all data-bearing runtime state in the main file. It has no data-bearing WAL, journal, or recovery sidecar. New extents are uncommitted main-file allocations until the inactive publication slot makes their complete root set authoritative. A lock sidecar may exist but contains no database data.

Protected extents refer directly to physical file ranges; v1 has no logical-page location map. Relocation therefore copies affected extents and rewrites their protected ancestor paths.

All backend-owned extents are self-framing, including B+ tree pages, overflow Values, Blob manifests, Blob chunks, and allocator structures. A child must agree with the independently protected location, bounds, generation, and role expected by its parent before any decoded content is released.

Suite 1 encrypts and authenticates application keys, Values, Blob content, and allocator content, but does not conceal Structural metadata or provide deniability. Suite 0 stores all of that content as plaintext and adds only publicly recomputable corruption checksums. Visible preambles expose unit roles, physical positions and spans, encoded and decoded sizes, compression representation, creation generations and logical sequences, and Blob ownership. Suite-1 preambles additionally expose key domains and nonces. Consequently an offline observer can always infer physical structure, approximate logical sizes, compression ratios, update generations, and which physical units belong to the same Blob.

## Common-region geometry

The first 64 KiB of every portable database file is the backend-independent common region:

| File range | Purpose |
|---|---|
| `0x0000`–`0x0fff` | Immutable 4 KiB visible bootstrap |
| `0x1000`–`0x1fff` | 4 KiB protected publication slot A |
| `0x2000`–`0x2fff` | 4 KiB protected publication slot B |
| `0x3000`–`0xffff` | Reserved, zero |
| `0x10000` onward | Backend-owned data region |

This geometry is independent of the capacity profile and aligns the backend data region to every permitted Allocation quantum. Backend extent block indices are absolute from file offset zero.

The bootstrap is immutable after creation. Commits alternate between the separately 4 KiB-aligned slots and never overwrite the slot representing the newest committed generation. New generation data becomes durable before the inactive slot publishes its root set. Open validates both slots with the protection mechanism selected by the bootstrap and selects the highest valid committed generation according to the interruption rules frozen by the recovery contract.

If a publication does not become authoritative, the prior slot's allocator root continues to classify reused extents as free, and bytes appended beyond its committed high-water mark are abandoned. Retired extents are persisted by generation until no live snapshot can observe them; after process loss no readers survive, so reopen may make all holds through the selected generation reclaimable under the recovery contract.

### Visible bootstrap

The 4 KiB visible bootstrap has this exact prefix and is zero-filled afterward:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | ASCII `MIAREDB\0` |
| 8 | 2 | Envelope version, `1` |
| 10 | 2 | Reserved, zero |
| 12 | 4 | Bootstrap length, `4096` |
| 16 | 4 | Common format version, `1` |
| 20 | 4 | Required envelope-feature bits, zero in v1 |
| 24 | 4 | KDF algorithm identifier |
| 28 | 4 | Derivation version |
| 32 | 4 | Encryption-suite identifier |
| 36 | 4 | Reserved, zero |
| 40 | 16 | Random database identity |
| 56 | 16 | KDF salt or zero |
| 72 | 8 | Common-region length, `65536` |
| 80 | 4 | Publication-slot length, `4096` |
| 84 | 4 | Publication-slot count, `2` |
| 88 | 8 | Backend-data offset, `65536` |
| 96 | 4000 | Reserved, zero |

The database identity is a random 128-bit value in both modes. Suite 1 also
stores the existing independently generated random 128-bit KDF salt, KDF
identifier 1, and derivation version 1. Suite 0 stores zeros in the salt, KDF,
and derivation fields. The complete bootstrap is protection context for both
publication slots. Only its fixed bounded prefix influences pre-validation
dispatch.

For suite 1, the first-stage keyed BLAKE2b-256 message is exactly 24 bytes: the 16-byte database identity, encryption-suite identifier as little-endian `u32`, and derivation version as little-endian `u32`. It uses the caller's exact 32 key bytes as the BLAKE2b key, the bootstrap's 16-byte KDF salt as the salt parameter, and the 16-byte personalization consisting of ASCII `MiareDbRootV1` followed by three zero bytes. The 32-byte output is the database root.

For suite 1, the second stage is libsodium-compatible `crypto_kdf_derive_from_key()` with the exact 8-byte context `MiareV1K`, 32-byte outputs, and subkey identifiers 1 for header, 2 for main data, 3 for recovery, and 4 for Blob data. No length prefix, magic, complete bootstrap, capacity value, or other field enters the first-stage message. Suite 0 performs neither stage and has no database root or domain keys.

Unknown magic, envelope version, or common format version produces `UnsupportedFormat`. Unknown required envelope features or encryption suite produces `UnsupportedFeature`. A suite-1 bootstrap with an unsupported KDF or derivation identity, or a suite-0 bootstrap with nonzero KDF, derivation, or salt fields, produces `IncompatibleProfile`. Once all suite-1 visible identities are supported, mutation affecting derivation or publication-slot authentication is indistinguishable from an incorrect key and produces `AuthenticationFailed` when no slot authenticates. Suite-0 checksum failure is corruption, never authentication failure.

### Publication-slot contents

Each publication payload is self-contained. Suite 1 encrypts it; suite 0 stores it as plaintext. It directly records the database identity, generation and predecessor generation; backend and backend-format identities; required and optional features; encryption, compression, codec, and profile identities; complete capacity-profile serialization and digest; ordered-keyspace, Blob-catalog, and allocator/reclamation root references; committed file high-water mark; and reserved space. A slot never relies on a separate configuration extent to identify or begin validating its committed generation.

V1 persists no session-open or clean-close marker. Every authoritative publication already names a complete main-file committed state, and opening does not perform a durable write merely to mark the session dirty. All v1 publication flags are zero.

Each 4 KiB publication slot has this visible 64-byte envelope:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | ASCII `MIARESLT` |
| 8 | 2 | Slot-envelope version, `1` |
| 10 | 2 | Slot index, `0` or `1` |
| 12 | 4 | Envelope length, `64` |
| 16 | 4 | Protected payload length, `4016` |
| 20 | 4 | Flags, zero |
| 24 | 24 | Suite-1 nonce or suite-0 zero bytes |
| 48 | 16 | Reserved, zero |
| 64 | 4016 | Suite-1 ciphertext or suite-0 plaintext |
| 4080 | 16 | Suite-1 authentication tag or suite-0 checksum |

For suite 1, the slot uses the derived header key. Its associated data remains exactly 4,176 bytes: the 16 bytes consisting of ASCII `MiareHeaderV1` followed by three zero bytes, the complete 4,096-byte visible bootstrap, and the complete 64-byte slot envelope. This is the released suite-1 encoding and does not change.

For suite 0, the 16-byte trailer is unkeyed BLAKE2b-128 over the exact
concatenation of the 16 bytes consisting of ASCII `MiareSlotCheckV1` followed
by zero padding, the complete bootstrap, the complete 64-byte envelope, and
the 4,016-byte plaintext payload. The nonce field is zero. The slot index must
match its physical offset in both modes. A structurally invalid or
protection-invalid slot is ignored when the other slot proves a committed
generation. If neither suite-1 slot authenticates, open returns
`AuthenticationFailed`; if neither suite-0 slot has a valid checksum, open or
verification reports `Corrupt`.

The protected slot payload is exactly 4,016 bytes:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | ASCII `MIAREPUB` |
| 8 | 2 | Publication version, `1` |
| 10 | 2 | Slot index |
| 12 | 4 | Plaintext length, `4016` |
| 16 | 8 | Generation |
| 24 | 8 | Predecessor generation |
| 32 | 16 | Database identity |
| 48 | 4 | Common format version |
| 52 | 4 | Storage-backend identifier |
| 56 | 4 | Backend-format version |
| 60 | 4 | Encryption-suite identifier |
| 64 | 8 | Required feature bits |
| 72 | 8 | Optional feature bits |
| 80 | 4 | Compression-policy identifier |
| 84 | 4 | Codec identifier |
| 88 | 4 | Codec-profile identifier |
| 92 | 4 | Capacity-profile version |
| 96 | 4 | Capacity-profile length |
| 100 | 4 | Reserved, zero |
| 104 | 32 | Capacity-profile digest |
| 136 | 104 | Capacity-profile bytes |
| 240 | 32 | Ordered-keyspace root reference |
| 272 | 32 | Blob-catalog root reference |
| 304 | 32 | Allocator/reclamation root reference |
| 336 | 8 | Committed high-water block count |
| 344 | 8 | Publication flags, zero |
| 352 | 3664 | Reserved, zero |

Generation starts at one with predecessor zero. Every later transaction or maintenance root publication increments generation by exactly one, records the prior generation as predecessor, and must not wrap. Initial creation writes both slots at generation one with identical semantic fields and matching slot indices. Suite 1 uses independent nonces; suite 0 uses zero nonce fields and distinct checksums because the physical slot index is covered. Null root references represent empty structures.

For every generation `g` at least two, the designated slot index is `g mod 2`: even generations use slot A/index zero and odd generations use slot B/index one. A suite-1 retry of an unpublished generation rewrites the same designated slot with a fresh nonce; a suite-0 retry deterministically recomputes its checksum. The other slot contains the authoritative predecessor when persistence begins.

The committed byte boundary is `high_water_blocks * allocationQuantumBytes`; reachable extents end within it. Protected slots normally hold equal initial generations or adjacent generations with a matching predecessor chain. Equal-generation slots must have identical semantic fields other than slot index. The valid newer generation wins. Contradictory protected slots are corruption; rollback of the complete file remains outside the threat model. Unknown required feature bits produce `UnsupportedFeature`; optional bits may be ignored only when they affect neither decoding nor semantics.

Fallback to the predecessor is permitted only when the designated newer slot is structurally invalid, torn, or fails its selected protection check. Once the newest slot passes that check, it establishes the committed generation: its canonical payload and all non-null roots receive framing, protection, role, generation, and shallow root-header validation during open. Failure beneath that publication is `Corrupt` or the applicable provider/resource error and never causes silent predecessor selection.

## Capacity-profile format parameters

`allocationQuantumBytes` is a power of two from 512 bytes through 64 KiB inclusive. `DefaultLimits` uses 4 KiB. Extent positions and allocation spans are expressed in whole allocation quanta. The value is persisted numerically in the protected capacity profile and must match the opening implementation exactly.

The framed-page ceiling is:

```text
max(16 KiB, allocationQuantumBytes)
```

The ceiling includes all page framing, encoded content, authentication data, and deterministic padding. An uncompressed page occupies the complete ceiling. A compressed page may occupy fewer whole allocation quanta. Plaintext capacity is the ceiling minus the exact framing overhead specified below.

Every B+ tree page has a fixed decoded payload size:

```text
pagePayloadBytes = framedPageCeiling - 160 - 16
```

The used page header, prefix, slots, and entries occupy the start of this image and every remaining decoded byte is zero. With the default 16 KiB ceiling, `pagePayloadBytes` is 16,208 bytes. An uncompressed page has equal stored and decoded payload lengths and exactly fills the framed-page ceiling.

When compression is selected for a page, its sole compression unit is the complete fixed decoded image, including zero free space. Authentication completes before decompression; bounded decompression must produce exactly `pagePayloadBytes` before page parsing begins. V1 does not split a page into separately compressed key, slot, or Value streams.

`maxInlineValueBytes` is an independent capacity-profile value and defaults to 1 KiB. Zero is valid and keeps only empty Values inline. The value must not exceed `maxValueBytes`, and a profile is invalid unless one leaf entry containing a maximum-length key and maximum inline Value plus all fixed metadata fits in one plaintext page.

`blobChunkBytes` is an independent capacity-profile value, defaults to 1 MiB, and is a power of two from 64 KiB through 16 MiB inclusive. It is at least `allocationQuantumBytes`. All permitted values are interoperability requirements, while only the 1 MiB default is performance-qualified.

### Canonical capacity-profile identity

The complete capacity-profile serialization is exactly 104 bytes:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | `allocationQuantumBytes` |
| 8 | 8 | `maxInlineValueBytes` |
| 16 | 8 | `blobChunkBytes` |
| 24 | 8 | `maxKeyBytes` |
| 32 | 8 | `maxValueBytes` |
| 40 | 8 | `maxBlobBytes` |
| 48 | 8 | `maxDatabaseBytes` |
| 56 | 8 | `maxKeyMutationsPerTransaction` |
| 64 | 8 | `maxBlobMutationsPerTransaction` |
| 72 | 8 | `maxBlobBytesPerTransaction` |
| 80 | 8 | `maxFileGrowthPerTransaction` |
| 88 | 4 | `maxCursorsPerTransaction` |
| 92 | 4 | `maxBlobReadersPerTransaction` |
| 96 | 4 | `maxOpenBlobWritersPerTransaction` |
| 100 | 4 | Reserved, zero |

Its identity is unkeyed BLAKE2b-256 over the exact concatenation of the 16 bytes consisting of ASCII `MiareLimitsV1` followed by three zero bytes, profile version `1` as little-endian `u32`, profile length `104` as little-endian `u32`, and the 104 canonical profile bytes. This digest is a protected compatibility identity rather than an authentication mechanism. A conforming opener compares the digest and every decoded field with its compile-time policy. Suite 1 retains the existing provider-compatible computation; suite 0 uses the byte-identical provider-independent project implementation.

## Primitive encoding

All multibyte integers are unsigned, fixed-width, and little-endian. Persistent data never contains a native C++ object representation or a signed integer. Byte strings are raw length-delimited bytes without terminators. Boolean facts use assigned flag bits. Enum, unit, algorithm, codec, and profile identifiers use explicitly assigned unsigned integer values.

File block indices, allocation spans, framed encoded lengths, logical Value and Blob lengths, file lengths, generations, and sequences use 64-bit integers. Page-local offsets and lengths use 32-bit integers. V1 uses no variable-length integers.

Every reserved bit and reserved or padding byte is canonically zero. A decoder rejects nonzero reserved data, unknown required flags, arithmetic overflow, non-minimal or inconsistent lengths, out-of-allocation references, overlapping fields, and values outside the protected capacity profile before allocating from or exposing decoded content.

### V1 identifier registry

Zero means `none` only where a field explicitly permits it and is otherwise invalid.

| Registry | Value | Meaning |
|---|---:|---|
| Storage backend | 1 | B+ tree |
| Encryption suite | 0 / 1 | None with fixed BLAKE2b-128 corruption checksum / XChaCha20-Poly1305-IETF |
| KDF algorithm | 0 / 1 | None / Miare BLAKE2b derivation v1 |
| Compression policy | 0 / 1 | None / Zstandard |
| Codec | 0 / 1 | None / RFC 8878 Zstandard |
| Codec profile | 0 / 1 | None / `ZstdV1` |
| Key domain | 0 | None for suite 0 |
| Key domain | 1 | Header |
| Key domain | 2 | Main data |
| Key domain | 3 | Recovery, reserved by this backend |
| Key domain | 4 | Blob data |
| Page type | 1 / 2 | Leaf / internal |
| Tree role | 1 | Ordered keyspace |
| Tree role | 2 | Blob catalog |
| Tree role | 3 | Blob chunk index |
| Tree role | 4 | Free extents |
| Tree role | 5 | Retired extents |

Visible extent unit kinds are:

| Value | Kind |
|---:|---|
| 1 | Ordered internal page |
| 2 | Ordered leaf page |
| 3 | Blob-catalog internal page |
| 4 | Blob-catalog leaf page |
| 5 | Blob-chunk-index internal page |
| 6 | Blob-chunk-index leaf page |
| 7 | Free-index internal page |
| 8 | Free-index leaf page |
| 9 | Retired-index internal page |
| 10 | Retired-index leaf page |
| 11 | Overflow Value |
| 12 | Blob manifest |
| 13 | Blob chunk |
| 14 | Allocator root |

Common format, backend format, envelope, publication, page, manifest, allocator, capacity-profile, and codec-profile versions all begin at one. Unknown identifiers are never inferred from surrounding structure. Suite identifier 0 itself fixes the checksum construction and domains specified above; V1 has no independent checksum-algorithm field or negotiable checksum profile.

## Extent references

An Extent reference is exactly 32 bytes:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | `block_index` |
| 8 | 8 | `block_count` |
| 16 | 8 | `encoded_length` |
| 24 | 8 | `creation_generation` |

The all-zero representation is the sole null reference; any partly zero representation is invalid. A non-null reference begins within the backend data region. `encoded_length` covers the complete self-framing extent but excludes allocation padding and must fit within `block_count * allocationQuantumBytes`. Every allocation-padding byte after `encoded_length` is zero.

The containing protected structure supplies the expected unit role. `creation_generation` is nonzero and no greater than the containing structure's generation. Reference arithmetic must not overflow or address outside the file, overlap a protected common region, or violate the protected capacity profile. Distinct reachable live extents do not overlap; an overlap is valid only where references are byte-for-byte identical. The child preamble and protection context must agree with the reference and expected role before decoded content is released.

## Protected-extent framing

Every backend Protected extent begins with this exact 160-byte visible preamble:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | ASCII `MIAREXT\0` |
| 8 | 2 | Preamble version, `1` |
| 10 | 2 | Unit-kind identifier |
| 12 | 4 | Flags |
| 16 | 4 | Key-domain identifier |
| 20 | 4 | Codec identifier |
| 24 | 4 | Codec-profile identifier |
| 28 | 4 | Preamble length, `160` |
| 32 | 8 | Physical block index |
| 40 | 8 | Allocated block count |
| 48 | 8 | Complete encoded length |
| 56 | 8 | Stored payload length |
| 64 | 8 | Decoded payload length |
| 72 | 8 | Creation generation |
| 80 | 8 | Logical sequence |
| 88 | 16 | Owner identity |
| 104 | 24 | Suite-1 nonce or suite-0 zero bytes |
| 128 | 32 | Reserved, zero |

The preamble is followed immediately by `stored_payload_length` protected
payload bytes, a 16-byte protection trailer, and zero allocation padding.
Consequently `encoded_length` is exactly
`160 + stored_payload_length + 16`. Suite 1 stores ciphertext and the existing
full authentication tag. Suite 0 stores plaintext and an unkeyed BLAKE2b-128
checksum. The fixed preamble is parsed and bounded before any provider call or
payload allocation but is not trusted until the selected protection check
succeeds.

Flags bit 0 is `compressed`; every other v1 flag bit is zero. An uncompressed unit has zero codec and profile identifiers and equal stored and decoded payload lengths. A compressed unit has the assigned Zstandard codec and profile identifiers and satisfies the minimum-savings rule specified below.

Blob manifests, Blob chunks, and Blob-chunk-index pages place their Blob identifier in `owner_identity`. Blob chunks place their zero-based ordinal in `logical_sequence`; every other v1 unit uses sequence zero. Other unit kinds use a zero owner identity. The preamble's position, span, encoded length, generation, and role must equal the containing reference and context. Suite 0 requires key domain zero and a zero nonce. Suite 1 retains its existing domain and nonce requirements.

### Associated data

The protection context for every backend extent is exactly 240 bytes:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 16 | ASCII `MiareExtentV1` followed by three zero bytes |
| 16 | 16 | Database identity |
| 32 | 4 | Common portable-format version |
| 36 | 4 | Storage-backend identifier |
| 40 | 4 | Backend-format version |
| 44 | 4 | Encryption-suite identifier |
| 48 | 32 | BLAKE2b-256 capacity-profile digest |
| 80 | 160 | Exact visible extent preamble |

All integer fields use canonical little-endian encoding. The profile digest covers the later-specified canonical serialization of every capacity-profile field. Parent bytes are not appended: protection of the parent establishes the expected reference and role, while this fixed context independently binds the child's database, formats, suite, profile, physical identity, generation, role, owner, sequence, lengths, representation, and nonce.

Suite 1 uses these 240 bytes as the released XChaCha20-Poly1305 associated
data without any change. Suite 0 computes its trailer as unkeyed BLAKE2b-128
over the exact concatenation of the 16-byte ASCII domain
`MiareExtentChkV1`, the 240-byte protection context, and the stored plaintext
payload. The checksum is deterministic and publicly recomputable. Validation
uses a full constant-time 16-byte comparison before decompression or structural
decoding, but this timing discipline does not turn the checksum into a
cryptographic authenticator.

### Suite-1 nonce and key-usage limits

Every encryption attempt draws all 24 nonce bytes independently from the provider CSPRNG. Retries and rewrites draw again even at the same location and generation. V1 never deliberately derives a nonce from an address, generation, or counter, persists no nonce ledger, and performs no probabilistic collision lookup.

One active database session performs fewer than `2^48` encryption attempts under each derived key domain. Generation is positive and must not reach `2^64`. Exhaustion is `ResourceLimit`. Each plaintext satisfies both the provider's XChaCha limit and the stricter unit and capacity-profile bound. A CSPRNG failure before persistence is `ProviderUnavailable`; after persistence begins it follows the fail-stop commit contract. Exact nonce reuse under one key is forbidden, while accidental collision risk relies on the required cryptographic CSPRNG and 192-bit nonce space.

Authentication failure releases no plaintext. Failure while establishing the encrypted publication is the intentionally conflated `AuthenticationFailed` outcome; after one publication authenticates and establishes database identity, failure of a reachable protected unit is `Corrupt` and enters recovery-required state. Suite 0 performs no nonce generation, KDF, encryption, or decryption; checksum failure likewise releases no decoded payload and is `Corrupt`, without any authentication claim.

### Zstandard codec profile 1

An eligible extent under the Zstandard compression policy is encoded as exactly one standard RFC 8878 frame at compression level 3 with a maximum 16 MiB window, known content size present, no dictionary or dictionary identifier, no content checksum, and no compression workers. Skippable frames, concatenated frames, and trailing bytes are forbidden. Provider identity, provider version, and exact compressed output are not persisted compatibility facts.

The compressed representation is selected exactly when its payload is smaller than the decoded payload and its complete framed extent occupies fewer Allocation quanta than the corresponding uncompressed extent. Otherwise the unit is stored uncompressed. Thus compression saves at least one allocation quantum; provider output may affect which valid representation is selected but never its logical interpretation.

The selected protection check completes before decompression. The decoder then requires one standard frame, the exact protected decoded content size, a window no larger than 16 MiB, no dictionary identifier or checksum, no trailing input, and exactly the expected bounded output before parsing or exposing content. B+ tree pages of every role, overflow Values, and Blob chunks are eligible. Blob manifests and fixed allocator roots are not.

## B+ tree topology

Internal separator `i` is the complete minimum key reachable from child `i + 1`; equality routes to the right child. Separators are full logical keys even though their page representation is prefix-compressed.

Leaf pages have no persistent sibling links. Forward and reverse cursors retain their protected root-to-leaf paths and cross leaf boundaries by ascending to an adjacent subtree and descending to its edge.

Every non-empty leaf stores the exact longest common prefix of its full keys once. Every internal page stores the exact longest common prefix of its full separators once. Indexed entries store independent suffixes, not chained front-coded values. Empty pages and separator-free internal pages use an empty prefix; a single entry may have an empty suffix.

### Decoded slotted-page image

An ordered-keyspace page begins with this 96-byte decoded header:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | ASCII `MIAREPG\0` |
| 8 | 2 | Page version, `1` |
| 10 | 2 | Page type, leaf or internal |
| 12 | 4 | Header length, `96` |
| 16 | 4 | Tree-role identifier |
| 20 | 4 | Level; leaves are zero |
| 24 | 4 | Entry count |
| 28 | 4 | Common-prefix length |
| 32 | 4 | Slot-array offset |
| 36 | 4 | Entry-area offset |
| 40 | 4 | Used length |
| 44 | 4 | Flags, zero |
| 48 | 32 | Internal page's leftmost child reference |
| 80 | 16 | Reserved, zero |

The header is followed by the common prefix, `entry_count` 8-byte slots, packed entries in key order, and zero free space. A slot is `entry_offset: u32` followed by `entry_length: u32`. The slot array begins immediately after the prefix, the entry area immediately after the slots, the first entry at the entry-area offset, and every later entry immediately after its predecessor. `used_length` is the byte after the final entry.

An internal entry is a `u32` key-suffix length, those suffix bytes, and the 32-byte right-child reference. The header supplies the leftmost child, so an internal page has one more child than separator entry. Sparse non-root internal pages may have one child and no separators. Every child is non-null, has level one below its parent, and agrees with the expected page role.

An ordered-keyspace leaf entry is a `u32` key-suffix length, those suffix bytes, a one-byte Value representation (`0` inline or `1` overflow), seven zero bytes, a `u64` logical Value length, and either exactly that many inline bytes or one 32-byte overflow-value reference. Leaf leftmost-child bytes are zero. The representation agrees exactly with the capacity profile's inline cutoff; child and overflow references are never null. The full key is the page prefix concatenated with the suffix, and reconstructed keys are strictly increasing.

The same decoded page container, prefix compression, slot representation, internal-entry schema, split algorithm, and lazy-deletion rules serve the ordered keyspace, Blob catalog, each per-Blob chunk index, free-extent index, and retired-extent index. Each tree role fixes its comparator and leaf payload schema. The ordered-keyspace inline cutoff does not govern metadata payloads, which are fixed inline structures.

The visible extent unit kind identifies both tree role and leaf/internal type, and the protected page header repeats that identity. A per-Blob chunk-index page also places its Blob identifier in the extent preamble's owner identity. Role disagreement at any layer is corruption.

An ordered-keyspace root is null exactly when that keyspace is empty; otherwise it references an ordered page at any level. The same null/empty equivalence applies to the Blob catalog. A page tree never represents emptiness with an allocated zero-entry leaf.

### Metadata leaf schemas

After the shared `u32` key-suffix length and suffix, metadata leaves use these exact payloads:

| Tree role | Full key and comparator | Leaf payload |
|---|---|---|
| Blob catalog | 16 raw Blob-ID bytes, unsigned-byte lexicographic | 32-byte manifest reference |
| Blob chunk index | `ordinal: u64 LE`, numeric | 32-byte chunk reference |
| Free extents | `start_block: u64 LE`, numeric | `block_count: u64 LE` |
| Retired extents | `retirement_generation: u64 LE` then `start_block: u64 LE`, numeric tuple | `block_count: u64 LE` |

Fixed-width keys reconstruct to exactly their stated size. References and block counts are nonzero. Free runs are disjoint and nonadjacent. Retired runs are disjoint and nonadjacent within one retirement generation. Chunk keys are dense according to their manifest, Blob catalog keys are unique, and internal pages use the same full-key representation and comparator as leaves of their role.

### Value overflow, splitting, and deletion

An inline Value resides completely in its ordered-keyspace leaf entry. A Value larger than `maxInlineValueBytes` resides in one independently protected overflow-value extent, optionally compressed as a single unit; its leaf entry stores the logical length and direct Extent reference. Blob storage, rather than overflow Values, provides chunked incremental access.

An overflow-value extent's decoded payload is exactly the raw Value bytes without an inner header. Suite 1 uses the main-data key domain; suite 0 uses key domain zero. Both use zero owner identity and logical sequence, and a decoded length equal to the protected leaf length, greater than `maxInlineValueBytes`, and no greater than `maxValueBytes`. Compression, when selected, covers the complete raw Value. Protection checking and bounded decompression finish before any Value bytes are returned.

Page overflow and splitting use canonical uncompressed plaintext sizes, never compressed sizes. Every boundary leaving at least one leaf entry or internal child on each side is evaluated after recomputing both pages' common-prefix encodings. Candidates that do not fit are discarded. The candidate with the smallest absolute size difference is chosen; ties select the smaller left count. The complete right-subtree minimum is propagated to the parent, and the same rule applies recursively.

Deletion does not redistribute or merge non-empty pages. Empty pages are removed recursively, a root with one child collapses to that child, and a tree that becomes empty uses its null root reference. Other non-empty pages have no minimum occupancy requirement.

## Blob representation

The Blob catalog is a shared slotted-page B+ tree ordered lexicographically by the 16 raw bytes of Blob identifier. Its leaf payload is exactly one non-null manifest Extent reference. Removing a Blob removes its catalog entry; v1 persists no catalog tombstone.

Every committed Blob content version has a dedicated manifest Protected extent. Suite 1 uses the Blob-derived key; suite 0 uses no key. The manifest preamble exposes the Blob identifier as owner, uses logical sequence zero, and its creation generation is the content-version generation. Replacement under a stable Blob identifier creates a new manifest and chunk index, then copy-on-write replaces the catalog reference; retained snapshots continue to reach the prior manifest. A rolled-back, failed, superseded, or deleted manifest becomes unreachable and follows ordinary generation-safe reclamation.

### Blob manifest

A manifest's decoded plaintext is exactly 128 bytes:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | ASCII `MIAREBLB` |
| 8 | 2 | Manifest version, `1` |
| 10 | 2 | Flags, zero |
| 12 | 4 | Manifest length, `128` |
| 16 | 16 | Blob identifier |
| 32 | 8 | Content generation |
| 40 | 8 | Logical Blob length |
| 48 | 8 | Chunk size |
| 56 | 8 | Chunk count |
| 64 | 32 | Chunk-index root reference |
| 96 | 32 | Reserved, zero |

Manifest extents are not compressed. The preamble owner equals the manifest Blob identifier, logical sequence is zero, and creation generation equals content generation. Chunk size equals the capacity profile. Chunk count is exactly the ceiling of logical length divided by chunk size. Length and count zero require a null chunk root; a non-empty Blob requires a non-null root. Logical length does not exceed `maxBlobBytes`.

V1 stores no whole-Blob checksum. Suite-1 manifest authentication or suite-0
manifest corruption checking binds identity, version, length, chunking, and
index root to that Protected unit. Each chunk independently protection-checks
its owner, ordinal, representation, and bytes. Only suite 1 provides
cryptographic authentication.

### Chunk index and chunk payload

A per-Blob chunk index uses exactly 8-byte little-endian `u64` ordinal keys with a numeric comparator. Canonical leaf keys are dense from zero through `chunk_count - 1`; internal separators are complete numeric right-subtree minima. The leaf payload is exactly one non-null 32-byte Blob-chunk Extent reference. Chunk-index page preambles expose the owning Blob identifier and use logical sequence zero.

A Blob-chunk extent uses the Blob key domain in suite 1 and key domain zero in
suite 0. It exposes the same owner identifier, records its zero-based ordinal
as logical sequence, and has the manifest content generation as creation
generation. Its decoded payload is exactly the raw Blob bytes without another
inner header. Every non-final chunk has `blobChunkBytes` decoded bytes; the
final length is the positive remainder implied by the manifest. Compression,
when selected, covers the complete raw chunk. The index key and reference,
preamble owner, ordinal and generation, and manifest-derived expected length
must all agree before content is exposed.

## Allocation and reclamation representation

One fixed allocator-root extent references two shared-format B+ tree indexes: free extents ordered by starting block, and retired extents ordered by the tuple of retirement generation and starting block. Both use numeric comparators over their fixed-width integer fields.

Free entries describe maximal coalesced block runs. Retired entries describe maximal coalesced runs sharing one retirement generation. An extent retired in generation `r` remains observable to snapshots older than `r` and becomes reusable exactly when no live snapshot has generation below `r`. After process loss no snapshots survive, so all retirement entries through the selected generation are eligible for reclamation.

The common region, reachable extents, free runs, retired runs, and tail at or beyond the committed high-water mark are disjoint. Tail space is not indexed. Allocator pages themselves use copy-on-write allocation and retirement. A null allocator root is valid only in the initial database when no backend block below the high-water mark is free or retired.

### Allocator root and index leaves

The uncompressed allocator-root decoded payload is exactly 160 bytes. Suite 1
uses the main-data key domain; suite 0 uses key domain zero:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | ASCII `MIAREALC` |
| 8 | 2 | Allocator version, `1` |
| 10 | 2 | Flags, zero |
| 12 | 4 | Root length, `160` |
| 16 | 8 | Root generation |
| 24 | 8 | High-water block count |
| 32 | 32 | Free-index root reference |
| 64 | 32 | Retired-index root reference |
| 96 | 8 | Reachable live block count |
| 104 | 8 | Free block count |
| 112 | 8 | Retired block count |
| 120 | 40 | Reserved, zero |

The preamble creation generation equals root generation, and the high-water mark equals the publication slot. Free-index keys and payloads are respectively an 8-byte numeric start block and 8-byte block count. Retired-index keys are the numeric tuple of 8-byte retirement generation and 8-byte start block; their payload is an 8-byte block count.

Counts include complete allocated spans and padding. The three protected counters partition every backend block below the high-water mark into reachable live, free, or retired state. They accelerate diagnostics but must equal a full verification traversal.

### Extent placement

Allocation uses lowest-address first-fit. For a required contiguous block count, the allocator scans free runs by ascending start block, consumes the low-address end of the first adequate run, and removes an exact match or retains its high-address suffix. If no free run fits, allocation appends at the transaction-local high-water mark. One extent never spans noncontiguous runs, and ordinary commit does not invoke compaction implicitly. Failure to find a run or grow within all profile limits is a preflight `ResourceLimit` outcome.

### Transaction-local staging and orphans

A write transaction owns a candidate allocator derived from the committed root; staging never changes committed allocation metadata. Reused runs are removed and appended tail blocks are added only in that candidate. Aborting a Blob writer or making staged structures unreachable returns their runs to the transaction-local free set, where suite 1 reuses them with fresh nonces and suite 0 reuses them with newly computed checksums and zero nonce fields.

Rollback or failure before publication discards the candidate: the committed root still classifies reused ranges as free, and appended bytes remain outside its high-water mark. Successful publication classifies every backend block below the new high-water mark as reachable, free, or retired. V1 has no persistent orphan catalog. After an uncertain publication, suite-selected protected-slot selection determines which complete allocator state is authoritative.

### Allocator metadata preflight

Allocator self-reference is resolved before persistence using a monotone reservation. After every non-allocator extent has an exact representation and block requirement, all superseded committed extents including prior allocator pages enter the candidate retired set. The implementation computes a conservative metadata-page reservation from exact entry counts and minimum uncompressed page fanout, allocates it through normal first-fit, and constructs the free and retired indexes at those assigned locations.

If more metadata pages are required, the reservation grows and construction repeats. Once sufficient, unused reserved suffixes return to the candidate free index and one final encoding fixes all roots, counters, protection trailers, byte representations, and the high-water mark; suite 1 also fixes fresh nonces. Reservation treats every metadata page as its full framed-page ceiling regardless of later compression. Failure to reach the profile-bounded fixed point is a preflight `ResourceLimit`; persistence-stage I/O has not begun.

## Compatibility and open behavior

Every format version is an opaque supported identifier rather than an ordered minimum. V1 supports exactly common format 1, B+ tree backend format 1, and the structure versions assigned by this document. An unlisted older or newer version is `UnsupportedFormat`; an implementation never attempts best-effort parsing from a numerically related version.

An unknown storage backend, encryption suite, or required feature is
`UnsupportedFeature`. An unknown KDF, derivation, or codec
profile, or any capacity-profile mismatch, is `IncompatibleProfile`. A known
required capability whose provider is absent is `ProviderUnavailable`. A keyed
entry point presented with suite 0 is `UnexpectedKey`; a keyless entry point
presented with suite 1 is `KeyRequired`. Unknown optional bits may be ignored
only when their definition affects neither parsing nor semantics, and an
implementation that does not understand them preserves them in every later
publication.

`open()` performs no migration, upgrade, normalization, or format-affecting rewrite. A later implementation that supports format 1 continues writing format 1 unchanged after opening it. The v1 release line retains read/write support for format 1. Any future deliberate compatibility break uses a separately named migration or application-level logical copy and otherwise rejects the file rather than mutating it implicitly. Forward compatibility exists only through explicitly ignorable optional bits.
