# Use stable bounded verification findings

Verification reports expose append-only machine-readable finding codes, severity, and bounded physical coordinates rather than implementation-defined messages or application identities. Reports retain the lexicographically first 64 findings, flag truncation, and continue all safely possible traversal so independent implementations can assert the same validity and observations without unbounded memory or leakage of keys, Values, Blob identifiers, content, cryptographic material, paths, or provider diagnostics.
