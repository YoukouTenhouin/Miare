# Separate keyed and keyless database entry points

Encrypted creation, open, and offline verification retain their existing
key-requiring APIs, while unencrypted operations use separately named keyless
entry points and an unencrypted creation-options type with no suite or key
field. A key is therefore neither nullable nor representable on the keyless
path, and encrypted callers cannot accidentally create suite 0 through an
option. Visible-suite mismatch produces stable `KeyRequired` or
`UnexpectedKey` errors; a wrong correctly sized suite-1 key remains
`AuthenticationFailed`. Provider sets own independently optional crypto and
compression capabilities, and the default keyless/no-compression path needs
neither.
