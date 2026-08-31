# Miare

Miare is a C++20 header-only embedded database for desktop applications. It
provides an ordered keyspace, snapshot transactions, transactional Blobs,
crash recovery, verification, backup, and online maintenance in one portable
database file. Encryption and compression are independent creation-time
choices.

The V1 behavior and file format are frozen and qualified across Linux, macOS,
and Windows. Existing encrypted V1 files remain byte-compatible. Miare project
code is header-only; libsodium and Zstandard are linked only when their system
providers are enabled.

## Requirements

- CMake 3.25 or newer and a C++20 compiler
- libsodium development headers and library when `MIARE_ENABLE_SODIUM=ON`
- Zstandard development headers and library when `MIARE_ENABLE_ZSTD=ON`

CMake enables both providers by default. Set either option to `OFF` to build
and package without that dependency. CMake searches for the enabled providers'
headers and libraries. Use
`CMAKE_PREFIX_PATH` when they are installed outside the default search paths,
or set `MIARE_SODIUM_INCLUDE_DIR`, `MIARE_SODIUM_LIBRARY`,
`MIARE_ZSTD_INCLUDE_DIR`, and `MIARE_ZSTD_LIBRARY` explicitly.

## Install and consume

Install Miare to a prefix:

```sh
cmake -S /path/to/miare -B build/miare \
  -DMIARE_BUILD_TESTS=OFF \
  -DMIARE_BUILD_EXAMPLES=OFF \
  -DCMAKE_INSTALL_PREFIX=/path/to/prefix
cmake --build build/miare --parallel
cmake --install build/miare
```

Consume the installed package from an application:

```cmake
find_package(miare CONFIG REQUIRED)
target_link_libraries(my_application PRIVATE miare::miare)
```

If the prefix is not searched automatically, configure the application with
`-DCMAKE_PREFIX_PATH=/path/to/prefix`.

Applications use one public header:

```cpp
#include <miare/database.hpp>
```

## First transaction

The smallest deployment is keyless and provider-free:

```cpp
auto database = miare::Database<>::createUnencrypted("people.miare");

{
    auto write = database.beginWrite();
    write.put(bytes("person:ada"), bytes("Ada Lovelace"));
    write.commit();
}

database.close();
```

This stores application content without confidentiality. Suite 0 uses
unkeyed checksums to detect accidental corruption, but an attacker can alter
content and recompute them.

For authenticated encryption, the application supplies exactly 32 bytes of
high-entropy key material. The following `loadDatabaseKey()` is deliberately
application-defined: production software should retrieve a generated key from
suitable secure storage, not use a password, a zero-filled array, or
command-line key material.

```cpp
std::array<std::byte, 32> keyBytes = loadDatabaseKey();
miare::EncryptionKeyView key{keyBytes};

auto database = miare::Database<>::create(
    "people.miare", key, miare::ProviderSet::system());

{
    auto write = database.beginWrite();
    write.put(bytes("person:ada"), bytes("Ada Lovelace"));
    write.commit();
}

{
    auto read = database.beginRead();
    if (auto value = read.get(bytes("person:ada"))) {
        use(*value);
    }
    read.end();
}

database.close();
```

Encrypted creation defaults to Zstandard compression and therefore uses both
system providers. Set `CreateOptions::compression` to `Compression::None` and
use `ProviderSet::systemCrypto()` for encryption without Zstandard. For a
keyless compressed database, set `UnencryptedCreateOptions::compression` to
`Compression::ZStd` and use `ProviderSet::systemCompression()`.

`bytes()` and `use()` above are application serialization helpers. Miare stores
arbitrary byte strings and intentionally does not impose a record format.

## Protection guarantees

| Persistent configuration | Confidentiality | Adversarial integrity | Accidental-corruption detection |
|---|---|---|---|
| Unencrypted, no compression | No | No | Unkeyed checksum and structure |
| Unencrypted, Zstandard | No | No | Unkeyed checksum and structure |
| XChaCha20-Poly1305, no compression | Yes | Authenticated protected units | Authentication and structure |
| XChaCha20-Poly1305, Zstandard | Yes | Authenticated protected units | Authentication and structure |

Compression provides no security property. Visible bootstrap and structural
metadata are not application content. Suite-0 checksums are publicly
recomputable, so successful verification is not proof of origin or resistance
to a malicious editor.

## Documentation

- [Getting started](docs/getting-started.md)
- [Transactions and ordered scans](docs/transactions.md)
- [Transactional Blobs](docs/blobs.md)
- [Operations, errors, and recovery](docs/operations.md)
- [Documentation index](docs/index.md)

Build the generated API reference locally with:

```sh
cmake -S . -B build/docs -DMIARE_BUILD_DOCS=ON \
  -DMIARE_BUILD_TESTS=OFF -DMIARE_BUILD_EXAMPLES=OFF
cmake --build build/docs --target miare_docs
```

Then open `build/docs/html/index.html`.

Complete, build-tested programs cover
[provider-free key/value transactions](https://github.com/YoukouTenhouin/Miare/blob/master/examples/key_value.cpp)
and [encrypted, compressed transactional Blobs](https://github.com/YoukouTenhouin/Miare/blob/master/examples/blob.cpp).
The frozen
[public transaction contract](docs/public-transaction-contract.md) and
[recovery and maintenance contract](docs/recovery-maintenance-verification-contract.md)
define precise V1 semantics.

## Important V1 boundaries

- One process and one open session may access a database file at a time.
- Values are bounded byte strings; large streaming content belongs in Blobs.
- Transactions and their cursors or Blob streams are thread-affine.
- Miare does not manage passwords, keychains, serialization, indexes,
  replication, rekeying, or salvage.
- Successful explicit `close()` establishes the clean single-file portability
  guarantee. Destruction performs only best-effort shutdown.

## Building the repository

```sh
cmake -S . -B build -DMIARE_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```
