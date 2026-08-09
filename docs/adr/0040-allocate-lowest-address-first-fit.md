# Allocate extents using lowest-address first-fit

For each contiguous extent request, the B+ tree allocator consumes the low end of the first adequate free run in ascending address order and appends at the high-water mark only when no run fits. This keeps one canonical address index, reuses early file space, and makes placement deterministic for a given request sequence, while accepting more fragmentation than best-fit; ordinary commits never hide that trade-off by invoking compaction implicitly.
