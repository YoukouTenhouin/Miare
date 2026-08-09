# Design the encryption envelope and provider contract

Type: grilling
Status: resolved
Blocked by: 05, 06

## Question

How should v1 derive and separate internal keys from caller-supplied key material, authenticate bootstrap metadata and backend data, construct nonces, bind locations and versions as associated data, handle wrong keys and tampering, erase sensitive memory, rotate keys if supported, and abstract vetted providers without weakening the on-disk security contract?

## Resolution

Resolved by the frozen [public contract](../../../docs/public-transaction-contract.md), [portable format](../../../docs/portable-btree-blob-format.md), and ADRs 0015, 0016, and 0018. They fix byte-exact BLAKE2b derivation, XChaCha envelopes, associated data, random nonces, usage bounds, fail-closed parsing, provider outcomes, and the exclusion of rekeying.
