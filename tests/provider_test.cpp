#include <miare/detail/providers.hpp>
#include <miare/testing/fakes.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

template<std::size_t Size>
std::array<std::byte, Size> sequence(unsigned first) {
    std::array<std::byte, Size> result{};
    for (std::size_t i = 0; i < Size; ++i) {
        result[i] = std::byte{static_cast<unsigned char>(first + i)};
    }
    return result;
}

std::vector<std::byte> bytes(std::string_view text) {
    const auto* first = reinterpret_cast<const std::byte*>(text.data());
    return {first, first + text.size()};
}

template<std::size_t Size>
std::array<std::byte, Size> hex(std::string_view text) {
    assert(text.size() == Size * 2);
    auto digit = [](char value) -> unsigned {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        return value - 'A' + 10;
    };
    std::array<std::byte, Size> result{};
    for (std::size_t i = 0; i < Size; ++i) {
        result[i] = std::byte{static_cast<unsigned char>(
            digit(text[i * 2]) * 16 + digit(text[i * 2 + 1]))};
    }
    return result;
}

} // namespace

int main() {
    using namespace miare;
    using namespace miare::detail;

    SodiumCryptoProvider crypto;

    const auto callerKey = sequence<32>(0);
    const auto identity = sequence<16>(16);
    const auto salt = sequence<16>(32);
    std::array<std::byte, cryptoKeyBytes> root{};
    crypto.deriveDatabaseRoot(callerKey, identity, salt, 1, 1, root);
    assert(root == hex<32>("85f9f40a640ba137de4482453b935bdc0f964109e1237baca3e9561dab11411d"));

    std::array<std::byte, cryptoKeyBytes> headerKey{};
    crypto.deriveSubkey(root, 1, headerKey);
    assert(headerKey == hex<32>("5569edef52f90dd89b36cac097b1377b9424c4f3c78c171046c06ced7f3c4d87"));
    try {
        crypto.deriveSubkey(root, 5, headerKey);
        assert(false);
    } catch (const ContractError& error) {
        assert(error.code() == Errc::InvalidArgument);
    }

    const auto aeadKey = sequence<32>(0x80);
    const auto nonce = hex<24>("07000000404142434445464748494a4b4c4d4e4f50515253");
    const auto associatedData = hex<12>("50515253c0c1c2c3c4c5c6c7");
    const auto plaintext = bytes(
        "Ladies and Gentlemen of the class of '99: If I could offer you only one "
        "tip for the future, sunscreen would be it.");
    const auto expected = hex<130>(
        "f8ebea4875044066fc162a0604e171feecfb3d20425248563bcfd5a155dcc47b"
        "bda70b86e5ab9b55002bd1274c02db35321acd7af8b2e2d25015e136b7679458"
        "e9f43243bf719d639badb5feac03f80a19a96ef10cb1d15333a837b90946ba38"
        "54ee74da3f2585efc7e1e170e17e15e563e77601f4f85cafa8e5877614e143e6"
        "8420");
    std::vector<std::byte> ciphertext(plaintext.size());
    std::array<std::byte, authenticationTagBytes> tag{};
    crypto.encryptDetached(
        aeadKey, nonce, plaintext, associatedData, ciphertext, tag);
    assert(std::equal(ciphertext.begin(), ciphertext.end(), expected.begin()));
    assert(std::equal(tag.begin(), tag.end(), expected.begin() + plaintext.size()));

    std::vector<std::byte> decrypted(plaintext.size(), std::byte{0xff});
    assert(crypto.decryptDetached(
        aeadKey, nonce, ciphertext, tag, associatedData, decrypted));
    assert(decrypted == plaintext);
    tag[0] ^= std::byte{1};
    std::fill(decrypted.begin(), decrypted.end(), std::byte{0xff});
    assert(!crypto.decryptDetached(
        aeadKey, nonce, ciphertext, tag, associatedData, decrypted));
    assert(std::ranges::all_of(decrypted, [](std::byte value) {
        return value == std::byte{0};
    }));

    try {
        const auto shortNonce = sequence<23>(0);
        crypto.encryptDetached(
            aeadKey, shortNonce, plaintext, associatedData, ciphertext, tag);
        assert(false);
    } catch (const ContractError& error) {
        assert(error.code() == Errc::InvalidArgument);
    }

    ZstdCompressionProvider compression;
    std::vector<std::byte> uncompressed(1024 * 1024, std::byte{0x2a});
    std::vector<std::byte> frame(compression.compressBound(uncompressed.size()));
    const auto frameSize = compression.compress(uncompressed, frame);
    frame.resize(frameSize);
    assert(frame.size() < uncompressed.size());
    std::vector<std::byte> decoded(uncompressed.size());
    compression.decompress(frame, decoded);
    assert(decoded == uncompressed);
    frame.push_back(std::byte{0});
    try {
        compression.decompress(frame, decoded);
        assert(false);
    } catch (const DatabaseError& error) {
        assert(error.code() == Errc::Corrupt);
    }
    try {
        (void)compression.compressBound(maxProviderUnitBytes + 1);
        assert(false);
    } catch (const ContractError& error) {
        assert(error.code() == Errc::InvalidArgument);
    }

    testing::FaultInjectingCompressionProvider faultingCompression;
    frame.resize(faultingCompression.compressBound(uncompressed.size()));
    faultingCompression.failNextProviderOperation();
    try {
        (void)faultingCompression.compress(uncompressed, frame);
        assert(false);
    } catch (const DatabaseError& error) {
        assert(error.code() == Errc::ProviderUnavailable);
    }
    faultingCompression.corruptNextFrame();
    frame.resize(faultingCompression.compress(uncompressed, frame));
    try {
        faultingCompression.decompress(frame, decoded);
        assert(false);
    } catch (const DatabaseError& error) {
        assert(error.code() == Errc::Corrupt);
    }

    testing::DeterministicCryptoProvider deterministic{7};
    std::array<std::byte, 4> randomA{};
    std::array<std::byte, 4> randomB{};
    deterministic.randomBytes(randomA);
    testing::DeterministicCryptoProvider replay{7};
    replay.randomBytes(randomB);
    assert(randomA == randomB);
    deterministic.failNextRandom();
    try {
        deterministic.randomBytes(randomA);
        assert(false);
    } catch (const DatabaseError& error) {
        assert(error.code() == Errc::ProviderUnavailable);
    }

    deterministic.failNextProviderOperation();
    try {
        deterministic.deriveSubkey(root, 1, headerKey);
        assert(false);
    } catch (const DatabaseError& error) {
        assert(error.code() == Errc::ProviderUnavailable);
    }

    deterministic.corruptNextCiphertext();
    tag.fill(std::byte{0});
    deterministic.encryptDetached(
        aeadKey, nonce, plaintext, associatedData, ciphertext, tag);
    std::fill(decrypted.begin(), decrypted.end(), std::byte{0xff});
    assert(!deterministic.decryptDetached(
        aeadKey, nonce, ciphertext, tag, associatedData, decrypted));

    auto providers = ProviderSet::system();
    (void)ProviderAccess::crypto(providers);
    (void)ProviderAccess::compression(providers);
    static_assert(!std::is_constructible_v<
                  ProviderSet,
                  std::unique_ptr<CryptoProvider>,
                  std::unique_ptr<CompressionProvider>>);
    auto testProviders = ProviderAccess::make(
        std::make_unique<testing::DeterministicCryptoProvider>(11),
        std::make_unique<testing::FaultInjectingCompressionProvider>());
    (void)ProviderAccess::crypto(testProviders);
}
