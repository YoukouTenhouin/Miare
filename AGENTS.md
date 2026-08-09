# Repository Guidelines

## Project Structure & Module Organization

Miare is a C++20 header-only embedded database. Public APIs live in `include/miare/`; keep implementation details in `include/miare/detail/` and test doubles in `include/miare/testing/`. Tests are standalone executables under `tests/`; `tests/package_consumer/` verifies installed-package consumption. Design contracts and decisions live in `docs/` and `docs/adr/`; consult them before changing persistence, transactions, recovery, cryptography, or compression. `CONTEXT.md` defines domain terminology.

## Build, Test, and Development Commands

Install development headers for libsodium and Zstandard, then use an out-of-tree build:

```sh
cmake -S . -B build -DMIARE_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

These commands configure, compile, and run all CTest cases. Isolate a case with `ctest --test-dir build -R miare.provider --output-on-failure`. CI validates Release builds on Linux, macOS, and Windows.

## Coding Style & Naming Conventions

Use four-space indentation, braces on the declaration line, `#pragma once`, and standard-library facilities where practical. Use `PascalCase` for types and enums, `camelCase` for functions and variables, and trailing underscores for private data members. Keep public declarations in `miare` and internal code in `miare::detail`. No formatter or linter is configured; match adjacent code.

## Testing Guidelines

Tests use small executable programs with standard `assert`, registered through CTest. Name new files `*_test.cpp`, add their executable and `add_test` entry to `tests/CMakeLists.txt`, and use deterministic fakes for fault or durability scenarios. Cover success paths, contract violations, provider failures, and byte-level compatibility where relevant. Run the full suite before submitting changes.

## Feature Development Workflow

For every new feature, create a Git-safe, lowercase kebab-case `<feature-name>-dev` branch. Commit frequently in small, logical increments. After the feature is complete and the full test suite passes, create `<feature-name>-reflow`. Reconstruct the work there as small, self-contained commits: each commit must include its relevant tests and fold later fixes into the commit that introduced the behavior, leaving the repository coherent at every step. The reflow history need not mirror development chronology, but its final tree must exactly match the dev branch. Verify this with `git diff --exit-code <feature-name>-dev <feature-name>-reflow`.

## Commit & Pull Request Guidelines

History follows Conventional Commit subjects such as `feat: establish header-only platform foundation` and `fix: support older Zstandard headers`. Keep subjects imperative and scoped to one logical change. Pull requests should explain behavioral impact, link the relevant issue or ADR, list verification commands, and call out portability or file-format implications.
