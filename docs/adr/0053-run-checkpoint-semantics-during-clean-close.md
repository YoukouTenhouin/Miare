# Run checkpoint semantics during clean close

After live-child checks and exclusive maintenance admission, B+ tree `close()` performs checkpoint semantics before releasing resources: it durably reclassifies safe retired runs and removes an abandoned tail, while publishing only if allocation state changes. It never compacts or performs full verification, and when no checkpoint work exists it issues no write or barrier; this gives clean close a bounded portable-file guarantee without turning routine shutdown into an unrequested whole-file operation.
