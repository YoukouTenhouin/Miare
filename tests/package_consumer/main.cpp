#include <miare/database.hpp>

#include <array>
#include <cstddef>

int main() {
    std::array<std::byte, 32> keyBytes{};
    const miare::EncryptionKeyView key{keyBytes};
    auto providers = miare::ProviderSet::system();
    (void)providers;
    return key.bytes().size() == 32 ? 0 : 1;
}
