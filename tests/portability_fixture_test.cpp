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
#include <utility>
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

enum class FixtureProtection {
    Unencrypted,
    Encrypted,
};

struct FixtureMode {
    FixtureProtection protection;
    miare::Compression compression;
    std::string_view suffix;
    std::size_t index;
    std::uint64_t seed;

    [[nodiscard]] bool encrypted() const noexcept {
        return protection == FixtureProtection::Encrypted;
    }
};

constexpr std::array fixtureModes{
    FixtureMode{FixtureProtection::Unencrypted, miare::Compression::None,
                "suite0-none", 0, 3},
    FixtureMode{FixtureProtection::Unencrypted, miare::Compression::ZStd,
                "suite0-zstd", 1, 4},
    FixtureMode{FixtureProtection::Encrypted, miare::Compression::None,
                "suite1-none", 2, 1},
    FixtureMode{FixtureProtection::Encrypted, miare::Compression::ZStd,
                "suite1-zstd", 3, 2},
};

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
    auto crypto =
        std::make_unique<miare::testing::DeterministicCryptoProvider>(seed);
    auto entropy =
        std::make_unique<miare::detail::CryptoEntropySource>(*crypto);
    return miare::detail::ProviderAccess::make(
        std::move(crypto),
        std::make_unique<miare::testing::FaultInjectingCompressionProvider>(),
        std::move(entropy));
}

void createFixture(
    const std::filesystem::path& path,
    const FixtureMode& mode) {
    auto database = [&] {
        if (mode.encrypted()) {
            miare::CreateOptions options;
            options.compression = mode.compression;
            return miare::Database<>::create(
                path,
                miare::EncryptionKeyView{encryptionKey},
                deterministicProviders(mode.seed),
                options);
        }
        miare::UnencryptedCreateOptions options;
        options.compression = mode.compression;
        return miare::Database<>::createUnencrypted(
            path, options, deterministicProviders(mode.seed));
    }();
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
    for (const auto& mode : fixtureModes) {
        createFixture(
            directory / (prefix + "-" + std::string{mode.suffix} + ".miare"),
            mode);
    }
    createUnsupportedFeatureFixture(
        directory / (prefix + "-suite1-zstd.miare"),
        directory / (prefix + "-unsupported.miare"));
}

void copyFixtureCorpus(
    const std::filesystem::path& source,
    const std::filesystem::path& destination) {
    assert(std::filesystem::create_directory(destination));
    for (const auto& entry : std::filesystem::directory_iterator{source}) {
        if (entry.is_regular_file() && entry.path().extension() == ".miare") {
            assert(std::filesystem::copy_file(
                entry.path(), destination / entry.path().filename()));
        }
    }
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

[[nodiscard]] std::vector<std::byte> readFile(
    const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary | std::ios::ate};
    assert(file);
    const auto size = file.tellg();
    assert(size > 0);
    std::vector<std::byte> image(static_cast<std::size_t>(size));
    file.seekg(0);
    file.read(
        reinterpret_cast<char*>(image.data()),
        static_cast<std::streamsize>(image.size()));
    assert(file);
    return image;
}

void qualifyFixtureBytes(
    const std::filesystem::path& path,
    const FixtureMode& mode) {
    const auto image = readFile(path);
    const miare::ByteView fileBytes{image};
    const auto expectedSuite = mode.encrypted()
        ? miare::detail::xchachaSuiteIdentifier
        : miare::detail::unencryptedSuiteIdentifier;
    assert(miare::detail::readLittleEndian<std::uint32_t>(
               fileBytes, miare::detail::BootstrapLayout::encryptionSuite) ==
           expectedSuite);
    assert(miare::detail::matches(fileBytes, 0, "MIAREDB\0"));

    const auto plaintext = std::string_view{"portable-value"};
    const auto foundPlaintext = std::search(
        image.begin(), image.end(),
        reinterpret_cast<const std::byte*>(plaintext.data()),
        reinterpret_cast<const std::byte*>(plaintext.data() + plaintext.size()));
    if (mode.encrypted()) {
        assert(foundPlaintext == image.end());
    } else if (mode.compression == miare::Compression::None) {
        assert(foundPlaintext != image.end());
    }

    if (!mode.encrypted()) {
        assert(miare::detail::readLittleEndian<std::uint32_t>(
                   fileBytes, miare::detail::BootstrapLayout::kdf) == 0);
        assert(miare::detail::readLittleEndian<std::uint32_t>(
                   fileBytes, miare::detail::BootstrapLayout::derivation) == 0);
        assert(miare::detail::allZero(
            fileBytes,
            miare::detail::BootstrapLayout::salt,
            miare::detail::BootstrapLayout::commonRegionLength));
        bool checkedPublication = false;
        for (std::size_t slot = 0; slot != 2; ++slot) {
            const auto offset = miare::detail::bootstrapBytes +
                slot * miare::detail::publicationSlotBytes;
            const auto plaintextOffset = offset +
                miare::detail::SlotEnvelopeLayout::ciphertext;
            if (!miare::detail::matches(
                    fileBytes, plaintextOffset, "MIAREPUB")) {
                continue;
            }
            assert(miare::detail::allZero(
                fileBytes,
                offset + miare::detail::SlotEnvelopeLayout::nonce,
                offset + miare::detail::SlotEnvelopeLayout::reserved));
            assert(miare::detail::readLittleEndian<std::uint32_t>(
                       fileBytes,
                       plaintextOffset +
                           miare::detail::PublicationLayout::compression) ==
                   static_cast<std::uint32_t>(mode.compression));
            checkedPublication = true;
        }
        assert(checkedPublication);
    }
}

[[nodiscard]] miare::Database<> openFixture(
    const std::filesystem::path& path,
    const FixtureMode& mode,
    std::uint64_t seed) {
    if (!mode.encrypted()) {
        return miare::Database<>::openUnencrypted(
            path, deterministicProviders(seed));
    }
    auto opened = miare::Database<>::open(
        path,
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(seed));
    assert(opened);
    return std::move(opened).value();
}

void qualifyCorruption(
    const std::filesystem::path& path,
    const FixtureMode& mode) {
    auto corruptPath = path;
    corruptPath += ".corrupt";
    assert(std::filesystem::copy_file(path, corruptPath));
    {
        std::fstream file{
            corruptPath, std::ios::binary | std::ios::in | std::ios::out};
        const auto offset = miare::detail::commonRegionBytes +
            miare::detail::ExtentLayout::bytes;
        file.seekg(static_cast<std::streamoff>(offset));
        char original = 0;
        file.get(original);
        file.seekp(static_cast<std::streamoff>(offset));
        file.put(static_cast<char>(original ^ 1));
        assert(file);
    }
    if (mode.encrypted()) {
        const auto report = miare::Database<>::verifyFile(
            corruptPath,
            miare::EncryptionKeyView{encryptionKey},
            deterministicProviders(30));
        assert(report && !report.value().valid);
        assert(report.value().findings.front().code ==
               miare::VerificationFindingCode::ExtentAuthenticationFailed);
    } else {
        const auto report = miare::Database<>::verifyUnencryptedFile(
            corruptPath, deterministicProviders(30));
        assert(!report.valid);
        assert(report.findings.front().code ==
               miare::VerificationFindingCode::ExtentChecksumFailed);
    }
    assert(std::filesystem::remove(corruptPath));
}

void qualifyTornInactivePublication(
    const std::filesystem::path& path,
    const FixtureMode& mode) {
    auto recoveryPath = path;
    recoveryPath += ".recovery";
    assert(std::filesystem::copy_file(path, recoveryPath));
    {
        constexpr auto offset = miare::detail::bootstrapBytes +
            miare::detail::publicationSlotBytes +
            miare::detail::SlotEnvelopeLayout::ciphertext + 127;
        std::fstream file{
            recoveryPath, std::ios::binary | std::ios::in | std::ios::out};
        std::array<char, 31> tornBytes{};
        tornBytes.fill(static_cast<char>(0xa5));
        file.seekp(static_cast<std::streamoff>(offset));
        file.write(tornBytes.data(), tornBytes.size());
        assert(file);
    }
    auto database = openFixture(recoveryPath, mode, 31);
    assert(database.diagnostics().rejectedInactivePublication);
    auto read = database.beginRead();
    assert(read.contains(bytes("inline")));
    read.end();
    database.close();
    assert(std::filesystem::remove(recoveryPath));
}

void consumeValidFixture(
    const std::filesystem::path& path,
    const FixtureMode& mode) {
    qualifyFixtureBytes(path, mode);
    qualifyCorruption(path, mode);
    qualifyTornInactivePublication(path, mode);

    if (mode.encrypted()) {
        const auto verified = miare::Database<>::verifyFile(
            path,
            miare::EncryptionKeyView{encryptionKey},
            deterministicProviders(10));
        assert(verified && verified.value().valid);
    } else {
        assert(miare::Database<>::verifyUnencryptedFile(
            path, deterministicProviders(10)).valid);
    }

    if (mode.encrypted()) {
        auto wrongKey = encryptionKey;
        wrongKey.front() ^= std::byte{1};
        const auto rejected = miare::Database<>::open(
            path,
            miare::EncryptionKeyView{wrongKey},
            deterministicProviders(11));
        assert(!rejected);
        assert(rejected.error() == miare::AuthenticationFailed{});
    } else {
        try {
            (void)miare::Database<>::open(
                path,
                miare::EncryptionKeyView{encryptionKey},
                deterministicProviders(11));
            assert(false);
        } catch (const miare::DatabaseError& error) {
            assert(error.code() == miare::Errc::UnexpectedKey);
        }
    }

    auto database = openFixture(path, mode, 12);
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
    database.checkpoint();
    database.compact();
    auto backup = path;
    backup += ".backup";
    const auto report = database.backupTo(backup);
    assert(report.sourceGeneration != 0);
    database.close();

    auto reopenedDatabase = openFixture(path, mode, 13);
    auto reopenedRead = reopenedDatabase.beginRead();
    const auto mutation = reopenedRead.get(bytes("consumer"));
    assert(mutation && std::equal(
        mutation->begin(), mutation->end(),
        bytes("mutated").begin(), bytes("mutated").end()));
    reopenedRead.end();
    reopenedDatabase.close();

    if (mode.encrypted()) {
        const auto backupVerification = miare::Database<>::verifyFile(
            backup,
            miare::EncryptionKeyView{encryptionKey},
            deterministicProviders(14));
        assert(backupVerification && backupVerification.value().valid);
    } else {
        assert(miare::Database<>::verifyUnencryptedFile(
            backup, deterministicProviders(14)).valid);
    }
    assert(std::filesystem::remove(backup));
}

void consumeFixtures(const std::filesystem::path& directory) {
    std::array<std::size_t, 4> validFixtures{};
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
        const auto name = entry.path().stem().string();
        const auto mode = std::find_if(
            fixtureModes.begin(), fixtureModes.end(),
            [&](const FixtureMode& candidate) {
                return name.ends_with(candidate.suffix);
            });
        assert(mode != fixtureModes.end());
        consumeValidFixture(entry.path(), *mode);
        ++validFixtures[mode->index];
    }
    assert(std::all_of(
        validFixtures.begin(), validFixtures.end(),
        [](std::size_t count) { return count != 0; }));
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
        temporary.path() / "local-suite1-none.miare");
    consumeFixtures(temporary.path());
    const auto versioned = temporary.path() / "versioned";
    copyFixtureCorpus(MIARE_VERSIONED_FIXTURE_DIRECTORY, versioned);
    consumeFixtures(versioned);
}
