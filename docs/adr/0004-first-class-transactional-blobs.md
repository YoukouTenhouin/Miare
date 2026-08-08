# Store large blobs through a distinct transactional facility

Large media assets must remain inside the portable database file so applications retain single-file portability and unified encryption and compression. V1 will therefore expose first-class transactional blobs with incremental I/O and chunked storage, while keeping ordinary values bounded and in-memory; values may hold database-local blob identifiers, and blob creation or removal participates atomically in the surrounding transaction.
