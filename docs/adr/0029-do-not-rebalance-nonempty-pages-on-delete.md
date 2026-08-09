# Leave non-empty B+ tree pages sparse after deletion

Deleting a key rewrites only its search path and never redistributes or merges non-empty siblings. Empty pages are removed recursively, a root with one child collapses to that child, and the empty database uses the canonical empty-tree representation; otherwise v1 imposes no minimum page occupancy. This preserves local copy-on-write mutation and makes variable-length deletion behavior deterministic, accepting sparse trees and temporary file amplification until explicit compaction rebuilds dense pages.
