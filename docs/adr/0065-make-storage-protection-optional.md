# Make encryption and compression orthogonal V1 capabilities

V1 supports independent creation-time encryption and compression choices: no
encryption with no compression, no encryption with Zstandard, XChaCha20-
Poly1305-IETF with no compression, and XChaCha20-Poly1305-IETF with Zstandard.
Neither choice can change for an existing file. Suite 1 retains its exact
released encoding, key requirements, and security guarantees; suite 0 is a
genuine keyless plaintext mode and never substitutes a dummy key or invokes a
KDF or AEAD operation. A disabled capability performs no corresponding
provider operation, and provider/build dependencies are independently
optional where the selected modes do not need them.
