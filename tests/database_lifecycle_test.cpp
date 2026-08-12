#include <miare/database.hpp>
#include <miare/testing/fakes.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto suffix = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        path_ = std::filesystem::temp_directory_path() /
            ("miare-database-lifecycle-" + suffix);
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

constexpr std::array<std::byte, 32> keyBytes{
    std::byte{0x00}, std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
    std::byte{0x04}, std::byte{0x05}, std::byte{0x06}, std::byte{0x07},
    std::byte{0x08}, std::byte{0x09}, std::byte{0x0a}, std::byte{0x0b},
    std::byte{0x0c}, std::byte{0x0d}, std::byte{0x0e}, std::byte{0x0f},
    std::byte{0x10}, std::byte{0x11}, std::byte{0x12}, std::byte{0x13},
    std::byte{0x14}, std::byte{0x15}, std::byte{0x16}, std::byte{0x17},
    std::byte{0x18}, std::byte{0x19}, std::byte{0x1a}, std::byte{0x1b},
    std::byte{0x1c}, std::byte{0x1d}, std::byte{0x1e}, std::byte{0x1f}};

struct AlternateLimits : miare::DefaultLimits {
    static constexpr std::uint64_t maxInlineValueBytes = 512;
};

static_assert(miare::LimitPolicy<AlternateLimits>);

[[nodiscard]] miare::ProviderSet deterministicProviders(
    std::uint64_t seed,
    bool withCompression = true) {
    std::unique_ptr<miare::detail::CompressionProvider> compression;
    if (withCompression) {
        compression = std::make_unique<miare::testing::FaultInjectingCompressionProvider>();
    }
    return miare::detail::ProviderAccess::make(
        std::make_unique<miare::testing::DeterministicCryptoProvider>(seed),
        std::move(compression));
}

[[nodiscard]] std::vector<std::byte> readFile(
    const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    const auto size = std::filesystem::file_size(path);
    std::vector<std::byte> bytes(size);
    input.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    assert(input);
    return bytes;
}

void writeByte(
    const std::filesystem::path& path,
    std::uint64_t offset,
    std::byte value) {
    std::fstream file{path, std::ios::binary | std::ios::in | std::ios::out};
    file.seekp(static_cast<std::streamoff>(offset));
    file.put(static_cast<char>(std::to_integer<unsigned char>(value)));
    assert(file);
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

template<class Operation>
void expectContractError(miare::Errc expected, Operation&& operation) {
    try {
        operation();
        assert(false);
    } catch (const miare::ContractError& error) {
        assert(error.code() == expected);
    }
}

void createCloseMoveAndReopen(const TemporaryDirectory& temporary) {
    const auto createdPath = temporary.path() / "created.miare";
    const auto movedPath = temporary.path() / "moved.miare";
    const miare::EncryptionKeyView key{keyBytes};

    auto database = miare::Database<>::create(
        createdPath, key, miare::ProviderSet::system());
    assert(database.state() == miare::DatabaseState::Open);
    database.close();
    assert(database.state() == miare::DatabaseState::Closed);
    assert(std::filesystem::file_size(createdPath) == 64U * 1024U);

    std::filesystem::rename(createdPath, movedPath);
    auto reopened = miare::Database<>::open(
        movedPath, key, miare::ProviderSet::system());
    assert(reopened.hasValue());
    assert(reopened.value().state() == miare::DatabaseState::Open);
    reopened.value().close();
}

void cleanCloseRemovesRolledBackBlobTail(
    const TemporaryDirectory& temporary) {
    const auto path = temporary.path() / "rolled-back-blob-tail.miare";
    auto database = miare::Database<>::create(
        path,
        miare::EncryptionKeyView{keyBytes},
        deterministicProviders(74));
    const auto committedBytes = std::filesystem::file_size(path);
    auto transaction = database.beginWrite();
    auto writer = transaction.createBlob();
    writer.write(std::vector<std::byte>(
        miare::DefaultLimits::blobChunkBytes, std::byte{0x74}));
    assert(std::filesystem::file_size(path) > committedBytes);
    writer.abort();
    transaction.rollback();

    database.close();
    assert(std::filesystem::file_size(path) == committedBytes);
}

void wrongKeyAndBootstrapTamperingAreAuthenticationFailures(
    const TemporaryDirectory& temporary) {
    const auto path = temporary.path() / "authentication.miare";
    auto database = miare::Database<>::create(
        path, miare::EncryptionKeyView{keyBytes}, deterministicProviders(11));
    database.close();

    auto wrongKey = keyBytes;
    wrongKey.front() ^= std::byte{1};
    auto rejected = miare::Database<>::open(
        path, miare::EncryptionKeyView{wrongKey}, deterministicProviders(12));
    assert(!rejected);
    assert(rejected.error() == miare::AuthenticationFailed{});

    writeByte(path, 100, std::byte{1});
    auto tamperedRejected = miare::Database<>::open(
        path, miare::EncryptionKeyView{keyBytes}, deterministicProviders(13));
    assert(!tamperedRejected);
}

void visibleCompatibilityFieldsHaveStableErrors(
    const TemporaryDirectory& temporary) {
    const miare::EncryptionKeyView key{keyBytes};
    const auto makeDatabase = [&](const char* name) {
        const auto path = temporary.path() / name;
        auto database = miare::Database<>::create(
            path, key, deterministicProviders(21));
        database.close();
        return path;
    };

    const auto formatPath = makeDatabase("unsupported-format.miare");
    writeByte(formatPath, 16, std::byte{2});
    expectDatabaseError(miare::Errc::UnsupportedFormat, [&] {
        (void)miare::Database<>::open(
            formatPath, key, deterministicProviders(22));
    });

    const auto featurePath = makeDatabase("unsupported-feature.miare");
    writeByte(featurePath, 20, std::byte{1});
    expectDatabaseError(miare::Errc::UnsupportedFeature, [&] {
        (void)miare::Database<>::open(
            featurePath, key, deterministicProviders(23));
    });

    const auto profilePath = makeDatabase("incompatible-profile.miare");
    writeByte(profilePath, 24, std::byte{2});
    expectDatabaseError(miare::Errc::IncompatibleProfile, [&] {
        (void)miare::Database<>::open(
            profilePath, key, deterministicProviders(24));
    });
}

void oneAuthenticPublicationSlotIsEnough(const TemporaryDirectory& temporary) {
    const auto path = temporary.path() / "one-slot.miare";
    const miare::EncryptionKeyView key{keyBytes};
    auto database = miare::Database<>::create(
        path, key, deterministicProviders(31));
    database.close();

    const auto bytes = readFile(path);
    writeByte(path, 4096 + 64, bytes[4096 + 64] ^ std::byte{1});
    auto reopened = miare::Database<>::open(path, key, deterministicProviders(32));
    assert(reopened);
    reopened.value().close();

    writeByte(path, 8192 + 64, bytes[8192 + 64] ^ std::byte{1});
    auto rejected = miare::Database<>::open(path, key, deterministicProviders(33));
    assert(!rejected);
}

void missingRequiredProviderFailsExplicitly(const TemporaryDirectory& temporary) {
    const auto path = temporary.path() / "provider.miare";
    const miare::EncryptionKeyView key{keyBytes};
    auto database = miare::Database<>::create(
        path, key, deterministicProviders(41));
    database.close();

    expectDatabaseError(miare::Errc::ProviderUnavailable, [&] {
        (void)miare::Database<>::open(
            path, key, deterministicProviders(42, false));
    });

    auto failingCrypto =
        std::make_unique<miare::testing::DeterministicCryptoProvider>(43);
    auto* failingCryptoView = failingCrypto.get();
    auto failingProviders = miare::detail::ProviderAccess::make(
        std::move(failingCrypto),
        std::make_unique<miare::testing::FaultInjectingCompressionProvider>());
    failingCryptoView->failNextProviderOperation();
    expectDatabaseError(miare::Errc::ProviderUnavailable, [&] {
        (void)miare::Database<>::open(path, key, std::move(failingProviders));
    });

    const auto uncompressedPath = temporary.path() / "no-codec-provider.miare";
    miare::CreateOptions options;
    options.compression = miare::Compression::None;
    auto uncompressed = miare::Database<>::create(
        uncompressedPath, key, deterministicProviders(44, false), options);
    uncompressed.close();
    auto reopened = miare::Database<>::open(
        uncompressedPath, key, deterministicProviders(45, false));
    assert(reopened);
    reopened.value().close();
}

void creationRejectsInvalidInputsWithoutReplacingFiles(
    const TemporaryDirectory& temporary) {
    const auto existingPath = temporary.path() / "existing.miare";
    {
        std::ofstream output{existingPath, std::ios::binary};
        output << "keep";
    }
    expectDatabaseError(miare::Errc::Io, [&] {
        (void)miare::Database<>::create(
            existingPath,
            miare::EncryptionKeyView{keyBytes},
            deterministicProviders(51));
    });
    const auto existingBytes = readFile(existingPath);
    assert(existingBytes.size() == 4);

    const auto invalidBackendPath = temporary.path() / "invalid-backend.miare";
    miare::CreateOptions invalidBackend;
    invalidBackend.storageBackend = static_cast<miare::StorageBackend>(99);
    expectContractError(miare::Errc::InvalidConfiguration, [&] {
        (void)miare::Database<>::create(
            invalidBackendPath,
            miare::EncryptionKeyView{keyBytes},
            deterministicProviders(52),
            invalidBackend);
    });
    assert(!std::filesystem::exists(invalidBackendPath));

    const auto shortKeyPath = temporary.path() / "short-key.miare";
    const std::array<std::byte, 31> shortKey{};
    expectContractError(miare::Errc::InvalidArgument, [&] {
        (void)miare::Database<>::create(
            shortKeyPath,
            miare::EncryptionKeyView{shortKey},
            deterministicProviders(53));
    });
    assert(!std::filesystem::exists(shortKeyPath));
}

void aDatabaseHasOnlyOneOpenSession(const TemporaryDirectory& temporary) {
    const auto path = temporary.path() / "in-use.miare";
    const miare::EncryptionKeyView key{keyBytes};
    auto database = miare::Database<>::create(
        path, key, deterministicProviders(61));
    expectDatabaseError(miare::Errc::InUse, [&] {
        (void)miare::Database<>::open(path, key, deterministicProviders(62));
    });

    const auto alias = temporary.path() / "in-use-alias.miare";
    std::error_code linkError;
    std::filesystem::create_hard_link(path, alias, linkError);
    if (!linkError) {
        expectDatabaseError(miare::Errc::InUse, [&] {
            (void)miare::Database<>::open(alias, key, deterministicProviders(63));
        });
    }
    database.close();
}

void concurrentCloseHasOneWinner(const TemporaryDirectory& temporary) {
    const auto path = temporary.path() / "concurrent-close.miare";
    auto database = miare::Database<>::create(
        path,
        miare::EncryptionKeyView{keyBytes},
        deterministicProviders(64));
    std::atomic<unsigned> ready{0};
    std::atomic<unsigned> successes{0};
    std::atomic<unsigned> invalidStates{0};
    const auto close = [&] {
        ready.fetch_add(1, std::memory_order_release);
        while (ready.load(std::memory_order_acquire) != 2) {
            std::this_thread::yield();
        }
        try {
            database.close();
            successes.fetch_add(1, std::memory_order_relaxed);
        } catch (const miare::ContractError& error) {
            assert(error.code() == miare::Errc::InvalidState);
            invalidStates.fetch_add(1, std::memory_order_relaxed);
        }
    };
    std::thread first{close};
    std::thread second{close};
    first.join();
    second.join();
    assert(successes.load() == 1);
    assert(invalidStates.load() == 1);
    assert(database.state() == miare::DatabaseState::Closed);
}

void capacityProfileIdentityIsImmutable(const TemporaryDirectory& temporary) {
    const auto path = temporary.path() / "alternate-profile.miare";
    using AlternateDatabase = miare::Database<std::allocator<std::byte>, AlternateLimits>;
    auto database = AlternateDatabase::create(
        path,
        miare::EncryptionKeyView{keyBytes},
        deterministicProviders(71));
    database.close();

    auto matching = AlternateDatabase::open(
        path,
        miare::EncryptionKeyView{keyBytes},
        deterministicProviders(72));
    assert(matching);
    matching.value().close();

    expectDatabaseError(miare::Errc::IncompatibleProfile, [&] {
        (void)miare::Database<>::open(
            path,
            miare::EncryptionKeyView{keyBytes},
            deterministicProviders(73));
    });
}

} // namespace

int main() {
    TemporaryDirectory temporary;
    createCloseMoveAndReopen(temporary);
    cleanCloseRemovesRolledBackBlobTail(temporary);
    wrongKeyAndBootstrapTamperingAreAuthenticationFailures(temporary);
    visibleCompatibilityFieldsHaveStableErrors(temporary);
    oneAuthenticPublicationSlotIsEnough(temporary);
    missingRequiredProviderFailsExplicitly(temporary);
    creationRejectsInvalidInputsWithoutReplacingFiles(temporary);
    aDatabaseHasOnlyOneOpenSession(temporary);
    concurrentCloseHasOneWinner(temporary);
    capacityProfileIdentityIsImmutable(temporary);
}
