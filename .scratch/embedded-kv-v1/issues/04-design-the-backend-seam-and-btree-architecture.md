# Design the backend seam and B+ tree architecture

Type: grilling
Status: open
Blocked by: 02, 03

## Question

Which responsibilities belong to the backend-independent transactional core versus a storage backend, and which B+ tree update, allocation, reclamation, and recovery architecture should v1 specify so that it satisfies the transaction contract without making a future LSM-tree backend unnatural?
