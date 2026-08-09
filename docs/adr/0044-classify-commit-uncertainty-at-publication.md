# Classify commit uncertainty at the first publication write

A persistence-stage commit failure is `CommitFailed` while the designated publication slot has not been touched, including failure to write or stabilize new extents. The first attempted write to that slot changes the outcome to `CommitOutcomeUnknown` until the second stable-storage barrier succeeds, because only reopen can then prove whether the predecessor or candidate generation became authoritative; an authenticated candidate with invalid reachable state is corruption, not grounds for fallback.
