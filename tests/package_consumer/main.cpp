#include <miare/database.hpp>
#include <miare/testing/fakes.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

namespace {

[[nodiscard]] miare::ByteView bytes(const std::string& value) {
    return {
        reinterpret_cast<const std::byte*>(value.data()),
        value.size()};
}

void putAndCheck(miare::Database<>& database) {
    const std::string key{"key"};
    const std::string value{"value"};
    auto write = database.beginWrite();
    write.put(bytes(key), bytes(value));
    write.commit();
    auto read = database.beginRead();
    const auto found = read.get(bytes(key));
    if (!found || found->size() != value.size()) {
        throw std::runtime_error{"package consumer read failed"};
    }
}

} // namespace

int main() {
    const auto suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto directory = std::filesystem::temp_directory_path() /
        ("miare-package-consumer-" + suffix);
    const auto plainPath = directory / "plain.miare";
    const auto backupPath = directory / "plain-backup.miare";
    std::filesystem::create_directory(directory);

    try {
        auto plain = miare::Database<>::createUnencrypted(plainPath);
        putAndCheck(plain);
        plain.checkpoint();
        plain.compact();
        (void)plain.backupTo(backupPath);
        plain.close();
        if (!miare::Database<>::verifyUnencryptedFile(plainPath).valid) {
            throw std::runtime_error{"plain package verification failed"};
        }
        auto reopenedPlain = miare::Database<>::openUnencrypted(plainPath);
        putAndCheck(reopenedPlain);
        reopenedPlain.close();

#if MIARE_HAS_ZSTD
        const auto compressedPath = directory / "compressed.miare";
        miare::UnencryptedCreateOptions compressedOptions;
        compressedOptions.compression = miare::Compression::ZStd;
        auto compressed = miare::Database<>::createUnencrypted(
            compressedPath,
            compressedOptions,
            miare::ProviderSet::systemCompression());
        putAndCheck(compressed);
        compressed.close();
        auto reopenedCompressed = miare::Database<>::openUnencrypted(
            compressedPath, miare::ProviderSet::systemCompression());
        putAndCheck(reopenedCompressed);
        reopenedCompressed.close();
#endif

#if MIARE_HAS_SODIUM
        const auto encryptedPath = directory / "encrypted.miare";
        std::array<std::byte, 32> keyBytes{};
        const miare::EncryptionKeyView key{keyBytes};
        miare::CreateOptions encryptedOptions;
        encryptedOptions.compression = miare::Compression::None;
        auto encrypted = miare::Database<>::create(
            encryptedPath,
            key,
            miare::ProviderSet::systemCrypto(),
            encryptedOptions);
        putAndCheck(encrypted);
        encrypted.close();
        auto reopenedEncrypted = miare::Database<>::open(
            encryptedPath, key, miare::ProviderSet::systemCrypto());
        if (!reopenedEncrypted) {
            throw std::runtime_error{"encrypted package open failed"};
        }
        putAndCheck(reopenedEncrypted.value());
        reopenedEncrypted.value().close();
#endif

#if MIARE_HAS_SODIUM && MIARE_HAS_ZSTD
        const auto protectedCompressedPath =
            directory / "protected-compressed.miare";
        auto protectedCompressed = miare::Database<>::create(
            protectedCompressedPath,
            key,
            miare::ProviderSet::system());
        putAndCheck(protectedCompressed);
        protectedCompressed.close();
        auto reopenedProtectedCompressed = miare::Database<>::open(
            protectedCompressedPath, key, miare::ProviderSet::system());
        if (!reopenedProtectedCompressed) {
            throw std::runtime_error{
                "protected compressed package open failed"};
        }
        putAndCheck(reopenedProtectedCompressed.value());
        reopenedProtectedCompressed.value().close();
#endif
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
        return 1;
    }

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    return 0;
}
