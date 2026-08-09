# Miare

Miare is a C++20 header-only embedded database implementation. The database
code is consumed through one target and one public header; its vetted
cryptography and compression providers remain linked dependencies.

```cmake
find_package(miare CONFIG REQUIRED)
target_link_libraries(my_application PRIVATE miare::miare)
```

```cpp
#include <miare/database.hpp>
```

The current foundation uses libsodium for XChaCha20-Poly1305-IETF and BLAKE2b,
and libzstd for Zstandard profile 1. Install both development packages before
configuring Miare. No separately built Miare library is produced.

For an in-tree build:

```sh
cmake -S . -B build -DMIARE_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```
