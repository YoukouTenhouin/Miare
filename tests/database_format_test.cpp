#include <miare/detail/database_format.hpp>
#include <miare/testing/fakes.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

int main() {
    constexpr std::array<std::byte, 32> keyBytes{
        std::byte{0x00}, std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
        std::byte{0x04}, std::byte{0x05}, std::byte{0x06}, std::byte{0x07},
        std::byte{0x08}, std::byte{0x09}, std::byte{0x0a}, std::byte{0x0b},
        std::byte{0x0c}, std::byte{0x0d}, std::byte{0x0e}, std::byte{0x0f},
        std::byte{0x10}, std::byte{0x11}, std::byte{0x12}, std::byte{0x13},
        std::byte{0x14}, std::byte{0x15}, std::byte{0x16}, std::byte{0x17},
        std::byte{0x18}, std::byte{0x19}, std::byte{0x1a}, std::byte{0x1b},
        std::byte{0x1c}, std::byte{0x1d}, std::byte{0x1e}, std::byte{0x1f}};
    miare::testing::DeterministicCryptoProvider deterministic{
        0x123456789abcdef0ULL};
    const auto image = miare::detail::makeInitialCommonRegion<miare::DefaultLimits>(
        miare::EncryptionKeyView{keyBytes},
        deterministic,
        miare::Compression::None);

    constexpr std::array<std::byte, 8> magic{
        std::byte{'M'}, std::byte{'I'}, std::byte{'A'}, std::byte{'R'},
        std::byte{'E'}, std::byte{'D'}, std::byte{'B'}, std::byte{0}};
    assert(std::equal(magic.begin(), magic.end(), image.begin()));
    assert(image[8] == std::byte{1});
    assert(image[12] == std::byte{0x00});
    assert(image[13] == std::byte{0x10});
    assert(std::all_of(
        image.begin() + 0x3000,
        image.end(),
        [](std::byte byte) { return byte == std::byte{0}; }));

    miare::detail::SodiumCryptoProvider crypto;
    std::array<std::byte, 32> digest{};
    crypto.hashBlake2b256(image, digest);
    constexpr std::array<std::byte, 32> expectedDigest{
        std::byte{0x25}, std::byte{0x81}, std::byte{0x54}, std::byte{0x2f},
        std::byte{0x3c}, std::byte{0xe0}, std::byte{0x0c}, std::byte{0xa5},
        std::byte{0x66}, std::byte{0x4d}, std::byte{0x12}, std::byte{0x30},
        std::byte{0x89}, std::byte{0x50}, std::byte{0x71}, std::byte{0x22},
        std::byte{0x10}, std::byte{0x1d}, std::byte{0x80}, std::byte{0x65},
        std::byte{0x5f}, std::byte{0x5a}, std::byte{0x91}, std::byte{0xa2},
        std::byte{0xea}, std::byte{0xd0}, std::byte{0x68}, std::byte{0x6f},
        std::byte{0x40}, std::byte{0x22}, std::byte{0xf3}, std::byte{0x4d}};
    assert(digest == expectedDigest);
}
