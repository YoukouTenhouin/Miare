# 14 — Deliver maintenance and portable backup operations

**What to build:** Give operators safe checkpoint, reclamation or compaction, online backup, integrity verification, and clean-close workflows that preserve committed data through interruption and insufficient space.

**Blocked by:** 13 — Harden recovery and fail-closed corruption handling.

**Status:** ready-for-agent

- [ ] Checkpointing consolidates eligible recovery state without invalidating live transactions or weakening the committed generation.
- [ ] Reclamation or compaction safely reduces unreachable storage subject to live snapshots and reports required or insufficient free space before destructive progress.
- [ ] Online backup produces a transactionally consistent destination that reopens independently with the specified key and configuration handling.
- [ ] Integrity verification authenticates and structurally checks all reachable tree, overflow, allocation, and Blob state with bounded resource use and actionable results.
- [ ] A successful clean close makes runtime data-bearing sidecars unnecessary, and interruption of any maintenance operation leaves the original committed database recoverable.
