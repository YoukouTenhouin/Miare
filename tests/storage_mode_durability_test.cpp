#include <miare/database.hpp>
#include <miare/testing/fakes.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::array<std::byte, 32> encryptionKey{};

struct StorageMode {
    bool encrypted;
    miare::Compression compression;
};

constexpr std::array storageModes{
    StorageMode{false, miare::Compression::None},
    StorageMode{false, miare::Compression::ZStd},
    StorageMode{true, miare::Compression::None},
    StorageMode{true, miare::Compression::ZStd},
};

[[nodiscard]] miare::ByteView bytes(std::string_view text) {
    return {
        reinterpret_cast<const std::byte*>(text.data()),
        text.size()};
}

[[nodiscard]] miare::ProviderSet providers(std::uint64_t seed) {
    auto crypto =
        std::make_unique<miare::testing::DeterministicCryptoProvider>(seed);
    auto entropy =
        std::make_unique<miare::detail::CryptoEntropySource>(*crypto);
    return miare::detail::ProviderAccess::make(
        std::move(crypto),
        std::make_unique<miare::testing::FaultInjectingCompressionProvider>(),
        std::move(entropy));
}

[[nodiscard]] miare::Database<> createDatabase(
    std::unique_ptr<miare::testing::MemoryDurableFile> file,
    const StorageMode& mode,
    std::uint64_t seed) {
    if (mode.encrypted) {
        miare::CreateOptions options;
        options.compression = mode.compression;
        return miare::testing::DatabaseAccess::create(
            std::move(file),
            miare::EncryptionKeyView{encryptionKey},
            providers(seed),
            options);
    }
    miare::UnencryptedCreateOptions options;
    options.compression = mode.compression;
    return miare::testing::DatabaseAccess::createUnencrypted(
        std::move(file), providers(seed), options);
}

[[nodiscard]] miare::Database<> openDatabase(
    std::unique_ptr<miare::testing::MemoryDurableFile> file,
    const StorageMode& mode,
    std::uint64_t seed) {
    if (!mode.encrypted) {
        return miare::testing::DatabaseAccess::openUnencrypted(
            std::move(file), providers(seed));
    }
    auto opened = miare::testing::DatabaseAccess::open(
        std::move(file),
        miare::EncryptionKeyView{encryptionKey},
        providers(seed));
    assert(opened);
    return std::move(opened).value();
}

void applyCandidate(miare::Database<>& database) {
    auto write = database.beginWrite();
    write.put(bytes("marker"), bytes("candidate"));
    write.put(bytes("anchor"), bytes("candidate"));
    write.put(
        bytes("payload"),
        std::vector<std::byte>(32U * 1024U, std::byte{0x5a}));
    write.commit();
}

[[nodiscard]] std::vector<std::byte> predecessorFixture(
    const StorageMode& mode,
    std::uint64_t seed) {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto* fileView = file.get();
    auto database = createDatabase(std::move(file), mode, seed);
    auto write = database.beginWrite();
    write.put(bytes("marker"), bytes("predecessor"));
    write.put(bytes("anchor"), bytes("predecessor"));
    write.commit();
    const auto image = fileView->bytes();
    database.close();
    return image;
}

void assertCompleteGeneration(
    const std::vector<std::byte>& image,
    const StorageMode& mode,
    std::uint64_t seed) {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    file->replaceStableBytes(image);
    auto database = openDatabase(std::move(file), mode, seed);
    auto read = database.beginRead();
    const auto marker = read.get(bytes("marker"));
    const auto anchor = read.get(bytes("anchor"));
    assert(marker && anchor);
    const bool predecessor = std::equal(
        marker->begin(), marker->end(),
        bytes("predecessor").begin(), bytes("predecessor").end());
    const bool candidate = std::equal(
        marker->begin(), marker->end(),
        bytes("candidate").begin(), bytes("candidate").end());
    assert(predecessor || candidate);
    assert(std::equal(marker->begin(), marker->end(), anchor->begin(), anchor->end()));
    assert(read.contains(bytes("payload")) == candidate);
    read.end();
    assert(database.verify().valid);
    database.close();
}

void everyCommitPersistenceOperationPreservesACompleteGeneration(
    const StorageMode& mode,
    std::uint64_t modeSeed) {
    const auto fixture = predecessorFixture(mode, modeSeed);
    auto traceFile = std::make_unique<miare::testing::MemoryDurableFile>();
    auto* traceView = traceFile.get();
    traceFile->replaceStableBytes(fixture);
    auto traceDatabase = openDatabase(
        std::move(traceFile), mode, modeSeed + 1);
    traceView->clearOperations();
    applyCandidate(traceDatabase);
    const auto operations = traceView->operations();
    traceDatabase.close();

    std::uint64_t seed = modeSeed + 10;
    for (std::size_t index = 0; index != operations.size(); ++index) {
        if (operations[index].kind ==
            miare::testing::DurableFileOperationKind::Read) {
            continue;
        }
        auto file = std::make_unique<miare::testing::MemoryDurableFile>();
        auto* fileView = file.get();
        file->replaceStableBytes(fixture);
        auto database = openDatabase(std::move(file), mode, seed++);
        fileView->clearOperations();
        fileView->failOperation(index, 0);
        try {
            applyCandidate(database);
            assert(false);
        } catch (const miare::DatabaseError&) {
        }
        fileView->simulateCrash();
        const auto crashImage = fileView->bytes();
        assertCompleteGeneration(crashImage, mode, seed++);
    }
}

} // namespace

int main() {
    std::uint64_t seed = 100;
    for (const auto& mode : storageModes) {
        everyCommitPersistenceOperationPreservesACompleteGeneration(mode, seed);
        seed += 1'000;
    }
}
