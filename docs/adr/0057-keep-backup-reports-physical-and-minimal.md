# Keep backup reports physical and minimal

A successful `BackupReport` exposes only source generation, destination bytes, verified extent and encoded-byte counts, live/free/retired block counts, whether the inactive publication was incomplete, and excluded abandoned-tail bytes. It omits paths, application object counts and identities, timings, throughput, provider diagnostics, native messages, and verification findings so backup remains a stable physical-fidelity operation rather than a second diagnostics or integrity-report interface.
