# Allow only one open session for a database file

A database file may have only one open `Database` session, which applications share across threads. The library rejects a second in-process open by resolved file identity and also takes the strongest practical exclusive operating-system lock to detect another process, reporting `DatabaseError` with `Errc::InUse`. The external lock remains defensive detection rather than a multi-process correctness promise, but rejecting duplicate sessions prevents two independent caches and writer coordinators from mutating the same file.
