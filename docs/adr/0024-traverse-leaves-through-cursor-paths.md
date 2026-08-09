# Traverse B+ tree leaves through retained cursor paths

V1 leaf pages contain no persistent next or previous sibling references. Cursors retain their authenticated root-to-leaf path and cross a leaf boundary by ascending to the adjacent subtree and descending to its edge. This makes boundary traversal proportional to tree height, but prevents a leaf split or relocation from forcing copy-on-write rewrites of otherwise unrelated neighboring leaves and their ancestor paths; both forward and reverse traversal retain the same local-update property.
