# Getting started

This guide installs Miare as a CMake package, chooses a storage-protection
mode, writes one transaction, and opens it again.

## Requirements and installation

Miare requires CMake 3.25+ and a C++20 compiler. The libsodium and Zstandard
development packages are required only when their providers are enabled. Both
are enabled by default; a provider-free installation uses:

```sh
cmake -S /path/to/miare -B build/miare \
  -DCMAKE_BUILD_TYPE=Release \
  -DMIARE_BUILD_TESTS=OFF \
  -DMIARE_BUILD_EXAMPLES=OFF \
  -DMIARE_ENABLE_SODIUM=OFF \
  -DMIARE_ENABLE_ZSTD=OFF \
  -DCMAKE_INSTALL_PREFIX=/path/to/prefix
cmake --build build/miare --parallel
cmake --install build/miare
```

Omit the two `MIARE_ENABLE_*` lines to install both providers. Either option can
be enabled independently. For nonstandard provider locations, add their
prefixes to `CMAKE_PREFIX_PATH` or set the four `MIARE_*_INCLUDE_DIR` and
`MIARE_*_LIBRARY` cache variables. Miare itself produces no compiled library.

In the consuming project:

```cmake
cmake_minimum_required(VERSION 3.25)
project(my_application LANGUAGES CXX)

find_package(miare CONFIG REQUIRED)
add_executable(my_application main.cpp)
target_link_libraries(my_application PRIVATE miare::miare)
```

Configure that project with `-DCMAKE_PREFIX_PATH=/path/to/prefix` if needed.
The imported target supplies the C++20 requirement, headers, and only the
enabled provider link dependencies.

## Choose a persistent mode

Encryption and compression are orthogonal and fixed when a database is
created:

| Encryption | Compression | Creation API | Required provider |
|---|---|---|---|
| None | None | `createUnencrypted(path)` | None |
| None | Zstandard | `createUnencrypted(path, options, ProviderSet::systemCompression())` | Zstandard |
| XChaCha20-Poly1305 | None | `create(path, key, ProviderSet::systemCrypto(), options)` | libsodium |
| XChaCha20-Poly1305 | Zstandard | `create(path, key, ProviderSet::system(), options)` | Both |

For the two non-default combinations, set the compression choice explicitly:

```cpp
miare::UnencryptedCreateOptions keylessCompressed;
keylessCompressed.compression = miare::Compression::ZStd;

miare::CreateOptions encryptedUncompressed;
encryptedUncompressed.compression = miare::Compression::None;
```

Opening and verification read the persisted compression choice. Use
`openUnencrypted` and `verifyUnencryptedFile` for suite 0, or the keyed `open`
and `verifyFile` functions for suite 1. Passing a key to a suite-0 file yields
`Errc::UnexpectedKey`; omitting one for suite 1 yields `Errc::KeyRequired`.

## Keys

`EncryptionSuite::XChaCha20Poly1305Ietf`, the encrypted V1 suite, requires exactly
32 bytes of high-entropy key material. `EncryptionKeyView` borrows those bytes
only during `create`, `open`, or `verifyFile`.

Generate a database key with a cryptographically secure random generator and
store it using an application-appropriate keychain or secret store. Miare does
not turn passwords into keys, store the caller key, integrate with keychains,
or support rekeying in V1. Losing the key loses access to the database.

The build-tested examples use libsodium to generate a temporary key that stays
in memory only for the program run. See
[`examples/example_support.hpp`](https://github.com/YoukouTenhouin/Miare/blob/master/examples/example_support.hpp).

## Byte strings

Miare accepts non-owning spans:

```cpp
using miare::ByteView;        // std::span<const std::byte>
using miare::MutableByteView; // std::span<std::byte>
```

Inputs are borrowed only for a call. `get()` returns allocator-aware owned
bytes, while cursor `key()` and `value()` return views tied to the cursor's
position and transaction. A small text adapter can be useful in an example:

```cpp
miare::ByteView bytes(std::string_view text) {
    return {
        reinterpret_cast<const std::byte*>(text.data()),
        text.size()};
}
```

This is only serialization at the application boundary; Miare does not assume
that stored bytes are text.

## Create, write, and close

Both creation functions require the target path not to exist and never
overwrite it. A provider-free, unencrypted database needs no key:

```cpp
auto database = miare::Database<>::createUnencrypted(databasePath);
```

An encrypted, Zstandard-compressed database uses both system providers:

```cpp
auto database = miare::Database<>::create(
    databasePath,
    miare::EncryptionKeyView{keyBytes},
    miare::ProviderSet::system());

{
    auto write = database.beginWrite();
    write.put(bytes("greeting"), bytes("hello"));
    write.commit();
}

database.close();
```

A committed transaction is atomic and durable. An active write transaction
rolls back when destroyed, but explicit `commit()` or `rollback()` makes intent
clear. End every transaction and close every cursor or Blob stream before
calling `Database::close()`; otherwise close throws `ContractError` with
`Errc::LiveChildren`.

Only successful explicit close guarantees that the portable database file no
longer needs data-bearing sidecars. Database destruction attempts best-effort,
non-throwing shutdown but is not a substitute when portability matters.

## Open an existing database

The keyless API opens suite-0 files directly:

```cpp
auto database = miare::Database<>::openUnencrypted(databasePath);
```

For encrypted files, `open()` separates bootstrap authentication rejection
from exceptional failures:

```cpp
auto opened = miare::Database<>::open(
    databasePath,
    miare::EncryptionKeyView{keyBytes},
    miare::ProviderSet::system());

if (!opened) {
    // Wrong key or an encrypted bootstrap that cannot authenticate.
    return;
}

auto database = std::move(opened).value();
{
    auto read = database.beginRead();
    if (auto value = read.get(bytes("greeting"))) {
        use(*value);
    }
    read.end();
}
database.close();
```

Opening may perform deterministic crash recovery, but it never upgrades the
format, changes the encryption or compression choices, or salvages partial
content. A second session for the same file fails with `Errc::InUse`.

## Creation and runtime options

`CreateOptions` fixes encrypted-file choices and defaults to
XChaCha20-Poly1305-IETF with Zstandard. `UnencryptedCreateOptions` cannot carry
a key or encryption suite and defaults to no compression. Both support the B+
tree backend and either Zstandard or no compression. These choices cannot be
changed on open.

Suite 1 encrypts application content and authenticates protected units against
adversarial changes. Suite 0 stores plaintext and uses publicly recomputable
BLAKE2b checksums for deterministic accidental-corruption detection; it makes
no confidentiality, authenticity, origin, or tamper-resistance claim.

`OpenOptions` controls runtime cache capacity and maximum concurrent readers.
Defaults are a 64 MiB cache and 256 readers. Runtime options do not change the
file format.

`Database<Allocator, Limits>` also accepts a stateful byte allocator and a
compile-time capacity profile. A database opens only with the exact `Limits`
profile that created it. The default profile is the performance-qualified V1
profile; custom profiles should be treated as an interoperability decision.

Continue with [transactions and ordered scans](transactions.md), or browse the
complete [`key_value.cpp`](https://github.com/YoukouTenhouin/Miare/blob/master/examples/key_value.cpp)
example.
