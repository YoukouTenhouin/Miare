#include <miare/database.hpp>
#include <miare/detail/blake2b.hpp>
#include <miare/testing/fakes.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        path_ = std::filesystem::temp_directory_path() /
            ("miare-unencrypted-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(path_);
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

[[nodiscard]] miare::ByteView bytes(const std::string& value) {
    return {
        reinterpret_cast<const std::byte*>(value.data()),
        value.size()};
}

[[nodiscard]] miare::ProviderSet compressionOnlyProviders() {
    return miare::detail::ProviderAccess::make(
        nullptr,
        std::make_unique<miare::testing::FaultInjectingCompressionProvider>());
}

[[nodiscard]] std::string text(miare::ByteView value) {
    return {
        reinterpret_cast<const char*>(value.data()),
        value.size()};
}

template<class Operation>
void expectDatabaseError(miare::Errc expected, Operation&& operation) {
    try {
        operation();
        assert(false);
    } catch (const miare::DatabaseError& error) {
        assert(error.code() == expected);
    }
}

[[nodiscard]] std::string hex(miare::ByteView bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        const auto value = std::to_integer<unsigned char>(byte);
        result.push_back(digits[value >> 4]);
        result.push_back(digits[value & 0x0f]);
    }
    return result;
}

[[nodiscard]] std::vector<std::byte> readFile(
    const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    std::vector<std::byte> result(std::filesystem::file_size(path));
    input.read(
        reinterpret_cast<char*>(result.data()),
        static_cast<std::streamsize>(result.size()));
    assert(input);
    return result;
}

void portableBlake2bChecksumsMatchKnownVectors() {
    const auto digest128 = miare::detail::blake2b<16>(miare::ByteView{});
    const auto digest256 = miare::detail::blake2b<32>(miare::ByteView{});
    assert(hex(digest128) == "cae66941d9efbd404e4d88758ea67670");
    assert(hex(digest256) ==
        "0e5751c026e543b2e8ab2eb06099daa1"
        "d1e5df47778f7787faab45cdf12fe3a8");

    std::array<std::byte, 1000> input{};
    for (std::size_t index = 0; index != input.size(); ++index) {
        input[index] = std::byte{static_cast<unsigned char>(index)};
    }
    const auto portable = miare::detail::blake2b<32>(input);
    std::array<std::byte, 32> provider{};
    miare::detail::SodiumCryptoProvider{}.hashBlake2b256(input, provider);
    assert(portable == provider);
}

void deterministicSuiteZeroBytesAreCanonical(
    const TemporaryDirectory& directory) {
    const auto path = directory.path() / "deterministic-plain.miare";
    auto providers = miare::detail::ProviderAccess::make(
        nullptr,
        nullptr,
        std::make_unique<miare::testing::DeterministicEntropySource>(0x12345678));
    auto database = miare::Database<>::createUnencrypted(
        path, {}, std::move(providers));
    {
        auto write = database.beginWrite();
        write.put(bytes(std::string{"fixture-key"}), bytes(std::string{"fixture-value"}));
        write.commit();
    }
    database.close();

    const auto file = readFile(path);
    const miare::ByteView input{file};
    assert(miare::detail::readLittleEndian<std::uint32_t>(
               input, miare::detail::BootstrapLayout::kdf) == 0);
    assert(miare::detail::readLittleEndian<std::uint32_t>(
               input, miare::detail::BootstrapLayout::derivation) == 0);
    assert(miare::detail::readLittleEndian<std::uint32_t>(
               input, miare::detail::BootstrapLayout::encryptionSuite) == 0);
    assert(miare::detail::allZero(
        input,
        miare::detail::BootstrapLayout::salt,
        miare::detail::BootstrapLayout::commonRegionLength));
    for (std::size_t slot = 0; slot != 2; ++slot) {
        const auto offset = miare::detail::bootstrapBytes +
            slot * miare::detail::publicationSlotBytes;
        assert(miare::detail::allZero(
            input,
            offset + miare::detail::SlotEnvelopeLayout::nonce,
            offset + miare::detail::SlotEnvelopeLayout::reserved));
        assert(miare::detail::matches(
            input,
            offset + miare::detail::SlotEnvelopeLayout::ciphertext,
            "MIAREPUB"));
    }
    const auto extent = miare::detail::commonRegionBytes;
    assert(miare::detail::readLittleEndian<std::uint32_t>(
               input, extent + miare::detail::ExtentLayout::keyDomain) == 0);
    assert(miare::detail::allZero(
        input,
        extent + miare::detail::ExtentLayout::nonce,
        extent + miare::detail::ExtentLayout::reserved));

    const auto digest = miare::detail::blake2b<32>(input);
    assert(hex(digest) ==
        "061e32c7b2435a3dcb9f4afaa61a4b91"
        "bbd12c20d4e399640341b537f441db71");
}

void entropyFailureLeavesNoFile(const TemporaryDirectory& directory) {
    const auto path = directory.path() / "entropy-failure.miare";
    auto entropy = std::make_unique<miare::testing::DeterministicEntropySource>(7);
    entropy->failNextOperation();
    auto providers = miare::detail::ProviderAccess::make(
        nullptr, nullptr, std::move(entropy));
    expectDatabaseError(miare::Errc::ProviderUnavailable, [&] {
        (void)miare::Database<>::createUnencrypted(
            path, {}, std::move(providers));
    });
    assert(!std::filesystem::exists(path));
}

void cryptoCapabilityIsNeverUsedBySuiteZero(
    const TemporaryDirectory& directory) {
    const auto path = directory.path() / "crypto-spy-plain.miare";
    const auto backupPath = directory.path() / "crypto-spy-backup.miare";
    auto calls = std::make_shared<std::atomic<std::size_t>>(0);
    const auto providersWithSpy = [&] {
        return miare::detail::ProviderAccess::make(
            std::make_unique<miare::testing::DeterministicCryptoProvider>(
                99, calls),
            nullptr);
    };
    auto database = miare::Database<>::createUnencrypted(
        path, {}, providersWithSpy());
    assert(!miare::testing::DatabaseAccess::sessionHasKeys(database));
    {
        auto write = database.beginWrite();
        auto blob = write.createBlob();
        blob.write(bytes(std::string{"no crypto operations"}));
        blob.finish();
        write.commit();
    }
    assert(database.verify().valid);
    database.checkpoint();
    database.compact();
    (void)database.backupTo(backupPath);
    database.close();
    auto reopened = miare::Database<>::openUnencrypted(
        path, providersWithSpy());
    reopened.close();
    assert(miare::Database<>::verifyUnencryptedFile(
        path, providersWithSpy()).valid);
    assert(miare::Database<>::verifyUnencryptedFile(
        backupPath, providersWithSpy()).valid);
    assert(calls->load(std::memory_order_relaxed) == 0);
}

void compressionFailuresAreExplicit(const TemporaryDirectory& directory) {
    miare::UnencryptedCreateOptions options;
    options.compression = miare::Compression::ZStd;
    const auto missingPath = directory.path() / "missing-compression.miare";
    expectDatabaseError(miare::Errc::ProviderUnavailable, [&] {
        (void)miare::Database<>::createUnencrypted(
            missingPath, options, miare::ProviderSet::none());
    });
    assert(!std::filesystem::exists(missingPath));

    const auto failingPath = directory.path() / "failing-compression.miare";
    auto compression =
        std::make_unique<miare::testing::FaultInjectingCompressionProvider>();
    auto* fault = compression.get();
    auto providers = miare::detail::ProviderAccess::make(
        nullptr, std::move(compression));
    auto database = miare::Database<>::createUnencrypted(
        failingPath, options, std::move(providers));
    fault->failNextProviderOperation();
    expectDatabaseError(miare::Errc::ProviderUnavailable, [&] {
        auto write = database.beginWrite();
        const std::string value(32U * 1024U, 'f');
        write.put(bytes(std::string{"failure"}), bytes(value));
        write.commit();
    });
    database.close();
}

void keylessDatabaseNeedsNoOptionalProvider(const TemporaryDirectory& directory) {
    const auto path = directory.path() / "plain.miare";
    const auto backupPath = directory.path() / "plain-backup.miare";
    const std::string key = "greeting";
    const std::string value = "suite zero stores this value as plaintext";
    const std::string blobValue = "blob bytes also use checksum protection";

    auto database = miare::Database<>::createUnencrypted(path);
    miare::BlobId blobId = [&] {
        auto write = database.beginWrite();
        write.put(bytes(key), bytes(value));
        auto blob = write.createBlob();
        const auto id = blob.id();
        blob.write(bytes(blobValue));
        blob.finish();
        write.commit();
        return id;
    }();
    assert(database.diagnostics().encryptionSuite == miare::EncryptionSuite::None);
    assert(database.verify().valid);
    database.checkpoint();
    database.compact();
    const auto backup = database.backupTo(backupPath);
    assert(backup.destinationFileBytes == std::filesystem::file_size(backupPath));
    database.close();

    auto reopened = miare::Database<>::openUnencrypted(path);
    {
        auto read = reopened.beginRead();
        const auto found = read.get(bytes(key));
        assert(found && text(*found) == value);
        auto openedBlob = read.openBlob(blobId);
        assert(openedBlob);
        std::vector<std::byte> content(blobValue.size());
        assert(openedBlob->read(content) == content.size());
        assert(text(content) == blobValue);
        openedBlob->close();
        read.end();
    }
    reopened.close();

    assert(miare::Database<>::verifyUnencryptedFile(path).valid);
    assert(miare::Database<>::verifyUnencryptedFile(backupPath).valid);

    std::ifstream input{path, std::ios::binary};
    const std::vector<char> fileBytes(
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{});
    assert(std::search(
               fileBytes.begin(),
               fileBytes.end(),
               value.begin(),
               value.end()) != fileBytes.end());
}

void suiteMismatchIsRejectedBeforeProviderUse(const TemporaryDirectory& directory) {
    const auto plainPath = directory.path() / "mismatch-plain.miare";
    auto plain = miare::Database<>::createUnencrypted(plainPath);
    plain.close();

    constexpr std::array<std::byte, 32> key{};
    expectDatabaseError(miare::Errc::UnexpectedKey, [&] {
        (void)miare::Database<>::open(
            plainPath,
            miare::EncryptionKeyView{key},
            miare::ProviderSet::none());
    });

    const auto rejectedCreate = directory.path() / "rejected-create.miare";
    miare::CreateOptions options;
    options.encryptionSuite = miare::EncryptionSuite::None;
    expectDatabaseError(miare::Errc::UnexpectedKey, [&] {
        (void)miare::Database<>::create(
            rejectedCreate,
            miare::EncryptionKeyView{key},
            miare::ProviderSet::none(),
            options);
    });
    assert(!std::filesystem::exists(rejectedCreate));

    const auto encryptedPath = directory.path() / "mismatch-encrypted.miare";
    auto encrypted = miare::Database<>::create(
        encryptedPath,
        miare::EncryptionKeyView{key},
        miare::ProviderSet::system());
    encrypted.close();
    expectDatabaseError(miare::Errc::KeyRequired, [&] {
        (void)miare::Database<>::openUnencrypted(
            encryptedPath, miare::ProviderSet::none());
    });
    expectDatabaseError(miare::Errc::KeyRequired, [&] {
        (void)miare::Database<>::verifyUnencryptedFile(
            encryptedPath, miare::ProviderSet::none());
    });
}

void unencryptedCompressionDoesNotRequireCrypto(const TemporaryDirectory& directory) {
    const auto path = directory.path() / "compressed-plain.miare";
    miare::UnencryptedCreateOptions options;
    options.compression = miare::Compression::ZStd;
    auto database = miare::Database<>::createUnencrypted(
        path, options, compressionOnlyProviders());
    const std::string value(32U * 1024U, 'z');
    {
        auto write = database.beginWrite();
        write.put(bytes(std::string{"compressed"}), bytes(value));
        write.commit();
    }
    assert(database.verify().valid);
    database.close();

    auto reopened = miare::Database<>::openUnencrypted(
        path, compressionOnlyProviders());
    auto read = reopened.beginRead();
    const auto found = read.get(bytes(std::string{"compressed"}));
    assert(found && text(*found) == value);
    read.end();
    reopened.close();
    assert(miare::Database<>::verifyUnencryptedFile(
        path, compressionOnlyProviders()).valid);
}

void checksumCorruptionIsReported(const TemporaryDirectory& directory) {
    const auto path = directory.path() / "corrupt.miare";
    auto database = miare::Database<>::createUnencrypted(path);
    {
        auto write = database.beginWrite();
        write.put(bytes(std::string{"key"}), bytes(std::string{"value"}));
        write.commit();
    }
    database.close();

    std::fstream file{path, std::ios::binary | std::ios::in | std::ios::out};
    file.seekg(static_cast<std::streamoff>(miare::detail::commonRegionBytes +
                                          miare::detail::ExtentLayout::bytes));
    char original = 0;
    file.get(original);
    file.seekp(static_cast<std::streamoff>(miare::detail::commonRegionBytes +
                                          miare::detail::ExtentLayout::bytes));
    file.put(static_cast<char>(original ^ 1));
    file.close();

    const auto report = miare::Database<>::verifyUnencryptedFile(path);
    assert(!report.valid);
    assert(!report.findings.empty());
    assert(report.findings.front().code ==
           miare::VerificationFindingCode::ExtentChecksumFailed);
}

void bothPublicationChecksumLossIsCorruption(
    const TemporaryDirectory& directory) {
    const auto path = directory.path() / "publication-loss.miare";
    auto database = miare::Database<>::createUnencrypted(path);
    database.close();

    std::fstream file{path, std::ios::binary | std::ios::in | std::ios::out};
    for (std::size_t slot = 0; slot != 2; ++slot) {
        const auto offset = miare::detail::bootstrapBytes +
            slot * miare::detail::publicationSlotBytes +
            miare::detail::SlotEnvelopeLayout::tag;
        file.seekg(static_cast<std::streamoff>(offset));
        char original = 0;
        file.get(original);
        file.seekp(static_cast<std::streamoff>(offset));
        file.put(static_cast<char>(original ^ 1));
    }
    file.close();

    assert(!miare::Database<>::verifyUnencryptedFile(path).valid);
    expectDatabaseError(miare::Errc::Corrupt, [&] {
        (void)miare::Database<>::openUnencrypted(path);
    });
}

void recoveryRejectsTornInactivePublication(
    const TemporaryDirectory& directory) {
    const auto path = directory.path() / "inactive-publication.miare";
    auto database = miare::Database<>::createUnencrypted(path);
    {
        auto write = database.beginWrite();
        write.put(bytes(std::string{"published"}), bytes(std::string{"yes"}));
        write.commit();
    }
    database.close();

    constexpr auto tornOffset = miare::detail::bootstrapBytes +
        miare::detail::publicationSlotBytes +
        miare::detail::SlotEnvelopeLayout::ciphertext + 127;
    std::fstream file{path, std::ios::binary | std::ios::in | std::ios::out};
    std::array<char, 31> tornBytes{};
    tornBytes.fill(static_cast<char>(0xa5));
    file.seekp(static_cast<std::streamoff>(tornOffset));
    file.write(tornBytes.data(), tornBytes.size());
    file.close();

    auto reopened = miare::Database<>::openUnencrypted(path);
    assert(reopened.diagnostics().rejectedInactivePublication);
    auto read = reopened.beginRead();
    const auto found = read.get(bytes(std::string{"published"}));
    assert(found && text(*found) == "yes");
    read.end();
    reopened.close();
}

} // namespace

int main() {
    portableBlake2bChecksumsMatchKnownVectors();
    const TemporaryDirectory directory;
    deterministicSuiteZeroBytesAreCanonical(directory);
    entropyFailureLeavesNoFile(directory);
    cryptoCapabilityIsNeverUsedBySuiteZero(directory);
    compressionFailuresAreExplicit(directory);
    keylessDatabaseNeedsNoOptionalProvider(directory);
    suiteMismatchIsRejectedBeforeProviderUse(directory);
    unencryptedCompressionDoesNotRequireCrypto(directory);
    checksumCorruptionIsReported(directory);
    bothPublicationChecksumLossIsCorruption(directory);
    recoveryRejectsTornInactivePublication(directory);
}
