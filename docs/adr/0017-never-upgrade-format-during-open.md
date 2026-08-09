# Keep format migration explicit

`open()` may perform deterministic crash recovery but never upgrades or migrates the portable format, storage backend, compression, encryption suite, or capacity profile. It opens a supported compatible file as-is and rejects unknown required features or incompatible identities with stable exceptions. Because migration can break backward compatibility and may require whole-file rewriting, any future migration must use a separately named workflow or an application-level logical copy rather than occurring as a side effect of access.

Format versions are opaque supported identities rather than ordered compatibility numbers. The v1 release line retains read/write support for format 1; a future implementation that supports it continues to publish that same format unchanged. Forward compatibility is limited to explicitly ignorable optional feature bits, which an implementation must preserve when it does not understand them.
