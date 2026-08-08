# Design transactional Blob semantics and storage

Type: grilling
Status: open
Blocked by: 01, 02, 04, 07, 09

## Question

What lifecycle, identifier, streaming, visibility, atomicity, overwrite, deletion, orphan handling, size, random-access, snapshot, compression, encryption, and failure semantics should Blobs expose, and how should the B+ tree backend store and reclaim Blob chunks inside the portable database file?
