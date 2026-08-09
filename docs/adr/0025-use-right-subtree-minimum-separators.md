# Use complete right-subtree minima as B+ tree separators

For every internal page, separator `i` is the complete smallest key reachable from child `i + 1`; equality therefore routes to the right child. Separators remain full logical keys even when their page representation uses prefix compression. This gives readers and verifiers one simple recursive invariant and avoids synthetic shortest-fence rules for arbitrary byte strings, accepting some additional separator storage and parent rewrites when a right subtree's minimum changes.
