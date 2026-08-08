# Keep format migration explicit

`open()` may perform deterministic crash recovery but never upgrades or migrates the portable format, storage backend, compression, encryption suite, or capacity profile. It opens a supported compatible file as-is and rejects unknown required features or incompatible identities with stable exceptions. Because migration can break backward compatibility and may require whole-file rewriting, any future migration must use a separately named workflow or an application-level logical copy rather than occurring as a side effect of access.
