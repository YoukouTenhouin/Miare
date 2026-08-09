# Verify authoritative state, not arbitrary storage residue

Full verification authenticates and structurally validates the selected committed graph, every graph retained by an online reader, and the allocator's complete range partition and counters. It does not authenticate payloads in free runs, offline-obsolete retired runs, or the abandoned tail because those bytes have no authoritative semantics; incomplete inactive publication and tail residue are nonfatal recovery observations, whereas any defect reachable beneath the authenticated selected publication makes the report invalid.
