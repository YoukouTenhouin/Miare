# Keep project code header-only, not provider dependencies

The database project's own C++ implementation will require no separately built database library, while cryptography and compression may use vetted vendored or linked providers. This preserves header-only integration for project code without forcing the project to implement security-sensitive primitives or impose a misleading zero-dependency constraint.
