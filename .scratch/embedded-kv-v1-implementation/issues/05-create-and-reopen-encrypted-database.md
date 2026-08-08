# 05 — Create and reopen an empty encrypted database

**What to build:** Let an application create, inspect, cleanly close, move, and reopen an empty authenticated portable database file using caller-supplied high-entropy key material.

**Blocked by:** 02 — Freeze the portable B+ tree and Blob formats; 04 — Establish the header-only platform and provider foundation.

**Status:** ready-for-agent

- [ ] Creation writes a self-identifying portable database file containing the selected storage backend, immutable creation choices, database identity, and authenticated empty state.
- [ ] A successful clean close leaves all committed state in the main file and reopening it on each supported operating system returns an empty ordered keyspace.
- [ ] Wrong keys, tampered bootstrap data, unsupported versions or features, unavailable cryptography, and incompatible backend requests fail with the specified errors.
- [ ] Caller key material is domain-separated as specified, and unauthenticated bootstrap plaintext never becomes observable.
- [ ] Header-only consumer and cross-platform fixture tests demonstrate the complete create, close, transfer, and reopen path.
