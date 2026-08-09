# Keep B+ tree crash recovery byte-preserving

Ordinary B+ tree `open()` selects and validates a committed generation without truncating the abandoned tail, rewriting allocation metadata, publishing a generation, or issuing a durability barrier. It may treat retirement holds made obsolete by process loss as reusable in its in-memory allocator view, but persists cleanup only with later successful work; this makes repeated recovery deterministic and prevents opening from requiring write capacity or introducing a new failure-prone durable transition.
