# Design the compression policy and provider contract

Type: grilling
Status: open
Blocked by: 04, 08

## Question

What must the database-creation compression option mean; how should the B+ tree decide whether and where to compress ordinary values and Blob chunks; how are codec identity, thresholds, bounds, errors, and future format evolution represented; and what provider interface preserves transparent reads without user-defined codecs?
