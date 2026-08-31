# Checksum unencrypted protected units without claiming authentication

Suite 0 retains the existing publication-slot and extent geometry but stores
plaintext payloads, zeroes nonce and key-domain fields, and writes a canonical
unkeyed BLAKE2b-128 checksum in the existing 16-byte trailer. The checksum
covers the same bounded structural context and stored payload that suite 1
authenticates, allowing deterministic corruption detection and one recovery,
verification, backup, and maintenance path without changing any suite-1 byte.
Because an attacker can recompute an unkeyed checksum, suite 0 provides no
confidentiality, cryptographic authentication, origin guarantee, or
adversarial tamper resistance; canonical structure checks do not strengthen
that security claim.
