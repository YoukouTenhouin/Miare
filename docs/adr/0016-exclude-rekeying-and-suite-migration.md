# Exclude rekeying and encryption-suite migration

Both changing encryption key material and changing the persisted encryption suite require rewriting every authenticated unit under a crash-safe migration protocol, so v1 supports neither operation on an existing database. The creation-time suite and key remain fixed for that file's lifetime. Applications needing rotation or a different suite must create a new database and copy logical keys, values, and Blobs through public transactions, keeping whole-file rewrite and migration policy outside the library.
