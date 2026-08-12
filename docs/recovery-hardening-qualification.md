# Recovery hardening qualification

Issue 13's deterministic qualification campaign is registered as
`miare.recovery_hardening`. It exercises the public database API over the
instrumented `DurableFile`, cryptography, compression, and allocator boundaries.
The campaign uses an independent literal predecessor/candidate model containing
a multi-level ordered tree, overflow Value, multi-chunk Blob, Blob indexes,
retirement metadata, and allocator root.

The campaign covers:

- interruption before every observed Blob-staging and commit `write`, `resize`,
  and stable-storage barrier operation;
- zero-, one-, 512-byte, Allocation-quantum, and final-byte write tears;
- loss, individual retention, full retention, and reverse replay of mutations
  preceding a failed first barrier;
- all 256 subsets of the eight 512-byte sectors in a publication slot;
- exact predecessor/candidate key, overflow Value, and Blob observations after
  every recovered image;
- wrong keys, rejected publication authentication, authenticated
  noncanonical state, unsupported format and feature identities, incompatible
  profiles, selected-boundary truncation, and provider failure as distinct
  public outcomes;
- framing, ciphertext, tag, and padding corruption for every reachable extent
  role in the fixture, plus duplicated/substituted and reordered extents; and
- a sentinel Blob-chunk read proving that a rejected authenticated unit copies
  no bytes to caller output before the session enters recovery-required state.

The B+ tree backend has no data-bearing recovery sidecar, so truncated-sidecar
enumeration is not applicable to this backend. Its only sidecar is a process
lock with no committed-state or recovery meaning.

The focused campaign complements permanent failure tests in
`miare.exact_transaction`, `miare.blob_transaction`,
`miare.backend_compression`, `miare.database_lifecycle`, and
`miare.provider`. Those tests cover transaction and Blob usage limits,
preflight nonce/randomness failure, allocator exhaustion, compression and
cryptography provider failures, terminal handles, and recovery-required close.
Together they require the previously committed generation to remain readable
after reopen and prohibit fallback to an unauthenticated or incompatible
representation.
