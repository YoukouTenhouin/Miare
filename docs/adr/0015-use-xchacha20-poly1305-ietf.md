# Use libsodium-compatible XChaCha20-Poly1305-IETF

V1 uses the libsodium-compatible XChaCha20-Poly1305-IETF construction rather than the previously assumed AES-256-GCM-SIV suite. Each independently authenticated unit uses a suite-selected 32-byte derived key, a fresh random 24-byte nonce, and the full 16-byte tag; purpose-separated derivation and canonical associated-data requirements remain. The extended nonce permits stateless random generation across crashes and independently evolving file copies and performs well without AES acceleration, while accepting that XChaCha is not a finalized IETF standard and that exact nonce reuse under one key remains forbidden; providers must reproduce the canonical construction and test vectors.

The format follows the [libsodium XChaCha20-Poly1305 construction](https://doc.libsodium.org/secret-key_cryptography/aead/chacha20-poly1305/xchacha20-poly1305_construction) and the archived [XChaCha specification](https://datatracker.ietf.org/doc/html/draft-irtf-cfrg-xchacha).

Every encryption attempt draws a fresh full 24-byte CSPRNG nonce, including retries at the same location and generation. V1 uses no durable nonce ledger or address-derived counter and limits one open session to fewer than `2^48` attempts per derived key domain, relying on XChaCha's random-nonce design rather than adding a pre-staging nonce-reservation publication.
