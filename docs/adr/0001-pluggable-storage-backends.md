# Select a storage backend when creating a database

The library will support multiple storage backends behind a common transactional key-value contract, with the backend selected permanently when a database is created. Backend-specific behavior such as whether compression applies per value or per immutable run remains behind that boundary; migrating an existing database between backends is explicitly unsupported, avoiding a cross-format migration contract while leaving room for B+ tree, LSM-tree, and future strategies.
