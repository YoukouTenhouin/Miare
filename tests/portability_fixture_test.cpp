#include <miare/database.hpp>
#include <miare/detail/database_format.hpp>
#include <miare/testing/fakes.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

constexpr std::array<std::byte, 32> encryptionKey{
    std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04},
    std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08},
    std::byte{0x09}, std::byte{0x0a}, std::byte{0x0b}, std::byte{0x0c},
    std::byte{0x0d}, std::byte{0x0e}, std::byte{0x0f}, std::byte{0x10},
    std::byte{0x11}, std::byte{0x12}, std::byte{0x13}, std::byte{0x14},
    std::byte{0x15}, std::byte{0x16}, std::byte{0x17}, std::byte{0x18},
    std::byte{0x19}, std::byte{0x1a}, std::byte{0x1b}, std::byte{0x1c},
    std::byte{0x1d}, std::byte{0x1e}, std::byte{0x1f}, std::byte{0x20}};

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        path_ = std::filesystem::temp_directory_path() /
            ("miare-portability-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        assert(std::filesystem::create_directory(path_));
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] miare::ByteView bytes(std::string_view text) {
    return {
        reinterpret_cast<const std::byte*>(text.data()),
        text.size()};
}

[[nodiscard]] miare::ProviderSet deterministicProviders(std::uint64_t seed) {
    return miare::detail::ProviderAccess::make(
        std::make_unique<miare::testing::DeterministicCryptoProvider>(seed),
        std::make_unique<miare::testing::FaultInjectingCompressionProvider>());
}

void createFixture(
    const std::filesystem::path& path,
    miare::Compression compression,
    std::uint64_t seed) {
    miare::CreateOptions options;
    options.compression = compression;
    auto database = miare::Database<>::create(
        path,
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(seed),
        options);
    auto write = database.beginWrite();
    write.put(bytes("empty"), miare::ByteView{});
    write.put(bytes("inline"), bytes("portable-value"));
    write.put(
        bytes("overflow"),
        std::vector<std::byte>(8'193, std::byte{0x5a}));
    auto blob = write.createBlob();
    const auto blobId = blob.id();
    blob.write(std::vector<std::byte>(
        miare::DefaultLimits::blobChunkBytes + 17,
        std::byte{0x6b}));
    blob.finish();
    write.put(bytes("blob-id"), blobId.toBytes());
    write.commit();
    database.close();
}

void createUnsupportedFeatureFixture(
    const std::filesystem::path& source,
    const std::filesystem::path& destination) {
    assert(std::filesystem::copy_file(source, destination));
    std::fstream file{
        destination,
        std::ios::in | std::ios::out | std::ios::binary};
    assert(file);
    constexpr std::uint32_t feature = 1;
    static_assert(
        miare::detail::BootstrapLayout::requiredFeatures + sizeof(feature) ==
        miare::detail::BootstrapLayout::kdf);
    std::array<char, sizeof(feature)> encoded{};
    for (std::size_t index = 0; index != encoded.size(); ++index) {
        encoded[index] = static_cast<char>(feature >> (index * 8U));
    }
    file.seekp(miare::detail::BootstrapLayout::requiredFeatures);
    file.write(encoded.data(), encoded.size());
    assert(file);
}

void produceFixtures(
    const std::filesystem::path& directory,
    std::string_view producer) {
    std::filesystem::create_directories(directory);
    const auto prefix = std::string{producer};
    const auto none = directory / (prefix + "-none.miare");
    const auto zstd = directory / (prefix + "-zstd.miare");
    createFixture(none, miare::Compression::None, 1);
    createFixture(zstd, miare::Compression::ZStd, 2);
    createUnsupportedFeatureFixture(
        zstd, directory / (prefix + "-unsupported.miare"));
}

void encryptedNoneFixtureRemainsByteCompatible(
    const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary | std::ios::ate};
    assert(file);
    const auto size = file.tellg();
    assert(size == 1'191'936);
    std::vector<std::byte> image(static_cast<std::size_t>(size));
    file.seekg(0);
    file.read(
        reinterpret_cast<char*>(image.data()),
        static_cast<std::streamsize>(image.size()));
    assert(file);

    miare::detail::SodiumCryptoProvider crypto;
    std::array<std::byte, 32> digest{};
    crypto.hashBlake2b256(image, digest);
    constexpr std::array<std::byte, 32> expected{
        std::byte{0x3e}, std::byte{0xec}, std::byte{0x0d}, std::byte{0xc2},
        std::byte{0x8a}, std::byte{0xf7}, std::byte{0x19}, std::byte{0xf4},
        std::byte{0x83}, std::byte{0x27}, std::byte{0x28}, std::byte{0x7a},
        std::byte{0xf2}, std::byte{0x8c}, std::byte{0xdb}, std::byte{0xf7},
        std::byte{0x5f}, std::byte{0xa5}, std::byte{0xbd}, std::byte{0xba},
        std::byte{0x9b}, std::byte{0x1a}, std::byte{0xfa}, std::byte{0xca},
        std::byte{0xb7}, std::byte{0x61}, std::byte{0xe1}, std::byte{0xdf},
        std::byte{0xf7}, std::byte{0x59}, std::byte{0x4b}, std::byte{0x44}};
    assert(digest == expected);
}

void consumeValidFixture(const std::filesystem::path& path) {
    const auto verified = miare::Database<>::verifyFile(
        path,
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(10));
    assert(verified && verified.value().valid);

    auto wrongKey = encryptionKey;
    wrongKey.front() ^= std::byte{1};
    const auto rejected = miare::Database<>::open(
        path,
        miare::EncryptionKeyView{wrongKey},
        deterministicProviders(11));
    assert(!rejected);
    assert(rejected.error() == miare::AuthenticationFailed{});

    auto opened = miare::Database<>::open(
        path,
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(12));
    assert(opened);
    auto database = std::move(opened).value();
    auto read = database.beginRead();
    const auto inlineValue = read.get(bytes("inline"));
    assert(inlineValue && std::equal(
        inlineValue->begin(), inlineValue->end(),
        bytes("portable-value").begin(), bytes("portable-value").end()));
    const auto overflow = read.get(bytes("overflow"));
    assert(overflow && overflow->size() == 8'193);
    const auto encodedId = read.get(bytes("blob-id"));
    assert(encodedId && encodedId->size() == miare::BlobId::encodedSize);
    std::array<std::byte, miare::BlobId::encodedSize> idBytes{};
    std::copy(encodedId->begin(), encodedId->end(), idBytes.begin());
    auto blob = read.openBlob(miare::BlobId::fromBytes(idBytes));
    assert(blob && blob->size() == miare::DefaultLimits::blobChunkBytes + 17);
    std::array<std::byte, 32> prefix{};
    assert(blob->read(prefix) == prefix.size());
    assert(std::all_of(
        prefix.begin(), prefix.end(),
        [](std::byte value) { return value == std::byte{0x6b}; }));
    blob->close();
    read.end();

    auto write = database.beginWrite();
    write.put(bytes("consumer"), bytes("mutated"));
    write.commit();
    auto backup = path;
    backup += ".backup";
    const auto report = database.backupTo(backup);
    assert(report.sourceGeneration != 0);
    database.close();

    auto reopened = miare::Database<>::open(
        path,
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(13));
    assert(reopened);
    auto reopenedDatabase = std::move(reopened).value();
    auto reopenedRead = reopenedDatabase.beginRead();
    const auto mutation = reopenedRead.get(bytes("consumer"));
    assert(mutation && std::equal(
        mutation->begin(), mutation->end(),
        bytes("mutated").begin(), bytes("mutated").end()));
    reopenedRead.end();
    reopenedDatabase.close();

    const auto backupVerification = miare::Database<>::verifyFile(
        backup,
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(14));
    assert(backupVerification && backupVerification.value().valid);
    assert(std::filesystem::remove(backup));
}

void consumeFixtures(const std::filesystem::path& directory) {
    std::size_t validFixtures = 0;
    std::size_t unsupportedFixtures = 0;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator{directory}) {
        if (!entry.is_regular_file() || entry.path().extension() != ".miare") {
            continue;
        }
        if (entry.path().stem().string().ends_with("-unsupported")) {
            try {
                (void)miare::Database<>::open(
                    entry.path(),
                    miare::EncryptionKeyView{encryptionKey},
                    deterministicProviders(15));
                assert(false);
            } catch (const miare::DatabaseError& error) {
                assert(error.code() == miare::Errc::UnsupportedFeature);
            }
            ++unsupportedFixtures;
            continue;
        }
        consumeValidFixture(entry.path());
        ++validFixtures;
    }
    assert(validFixtures != 0);
    assert(unsupportedFixtures != 0);
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 4 && std::string_view{argv[1]} == "--produce") {
        produceFixtures(argv[2], argv[3]);
        return 0;
    }
    if (argc == 3 && std::string_view{argv[1]} == "--consume") {
        consumeFixtures(argv[2]);
        return 0;
    }
    if (argc != 1) {
        return 2;
    }
    TemporaryDirectory temporary;
    produceFixtures(temporary.path(), "local");
    encryptedNoneFixtureRemainsByteCompatible(
        temporary.path() / "local-none.miare");
    consumeFixtures(temporary.path());
}
