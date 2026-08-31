# Define single-file storage at the clean-close boundary

“Single-file” guarantees that a cleanly closed database can be copied or moved using only its main file; it does not prohibit runtime WAL, journal, or lock sidecars. Any sidecar containing database data must receive protection equivalent to the database's selected suite, and a successful close must leave all committed state self-contained in the portable database file, preserving simple transfer without forcing every backend into an embedded-journal design.
