# 05 — Create and reopen an empty encrypted database

**What to build:** Let an application create, inspect, cleanly close, move, and reopen an empty authenticated portable database file using caller-supplied high-entropy key material.

**Blocked by:** 02 — Freeze the portable B+ tree and Blob formats; 04 — Establish the header-only platform and provider foundation.

**Status:** resolved

- [x] Creation writes a self-identifying portable database file containing the selected storage backend, immutable creation choices, database identity, and authenticated empty state.
- [x] A successful clean close leaves all committed state in the main file and reopening it on each supported operating system returns an empty ordered keyspace.
- [x] Wrong keys, tampered bootstrap data, unsupported versions or features, unavailable cryptography, and incompatible backend requests fail with the specified errors.
- [x] Caller key material is domain-separated as specified, and unauthenticated bootstrap plaintext never becomes observable.
- [x] Header-only consumer and cross-platform fixture tests demonstrate the complete create, close, transfer, and reopen path.

## Resolution

`Database::create` and `Database::open` now implement the frozen v1 common
region: the visible bootstrap, two independently encrypted generation-one
publication slots, canonical capacity-profile identity, null empty-state roots,
and the exact committed 64 KiB boundary. Creation uses an exclusive sibling
temporary file, a stable-storage barrier, reopen validation, exclusive namespace
installation, and final-path validation. Explicit clean close releases the
portable main file without an unnecessary publication or barrier.

Opening performs bounded bootstrap dispatch, derives all four session key
domains, authenticates both publication slots independently, validates canonical
authenticated fields and immutable choices, selects a surviving publication,
and enforces the committed file boundary. Wrong keys and supported-bootstrap
tampering share the `AuthenticationFailed` outcome; format, feature, profile,
provider, duplicate-session, and corruption paths retain their stable categories.
Session keys receive best-effort erasure on close, failure, move, and destruction.

The lifecycle suite covers create, close, filesystem transfer, reopen, wrong
keys, bootstrap tampering, single- and double-slot damage, provider availability,
profile mismatch, invalid creation choices, exclusive creation, and in-use
detection. A deterministic full-file BLAKE2b-256 fixture locks the canonical
portable bytes, all key-domain outputs have fixed vectors, and the installed
header-only package consumer executes the complete lifecycle.
