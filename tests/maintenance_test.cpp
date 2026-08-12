#include <miare/database.hpp>
#include <miare/testing/fakes.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::array<std::byte, 32> encryptionKey{};

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        path_ = std::filesystem::temp_directory_path() /
            ("miare-maintenance-" + std::to_string(
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

void checkpointRespectsSnapshotsAndPersistsReclamation() {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto* fileView = file.get();
    auto database = miare::testing::DatabaseAccess::create(
        std::move(file),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(1));
    auto seed = database.beginWrite();
    seed.put(bytes("key"), std::vector<std::byte>(8'193, std::byte{0x31}));
    seed.commit();
    auto retained = database.beginRead();
    auto replacement = database.beginWrite();
    replacement.put(bytes("key"), bytes("replacement"));
    replacement.commit();

    const auto before = database.diagnostics();
    assert(before.snapshotRetainedBytes > 0);
    database.checkpoint();
    assert(database.diagnostics().snapshotRetainedBytes ==
        before.snapshotRetainedBytes);
    retained.end();

    database.checkpoint();
    const auto reclaimed = database.diagnostics();
    assert(reclaimed.reclaimableBytes <= before.snapshotRetainedBytes);
    assert(reclaimed.snapshotRetainedBytes == 0);
    database.close();

    auto emptyFile = std::make_unique<miare::testing::MemoryDurableFile>();
    auto* emptyFileView = emptyFile.get();
    auto empty = miare::testing::DatabaseAccess::create(
        std::move(emptyFile),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(10));
    emptyFileView->clearOperations();
    empty.checkpoint();
    assert(emptyFileView->operations().empty());
    empty.close();
}

void verificationIsBoundedAndFailsClosed() {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto* fileView = file.get();
    auto database = miare::testing::DatabaseAccess::create(
        std::move(file),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(2));
    auto write = database.beginWrite();
    write.put(bytes("key"), bytes("value"));
    auto blob = write.createBlob();
    blob.write(bytes("blob"));
    blob.finish();
    write.commit();

    const auto healthy = database.verify();
    assert(healthy.valid);
    assert(healthy.selectedGeneration == 2);
    assert(healthy.keysChecked == 1);
    assert(healthy.blobsChecked == 1);
    assert(healthy.blobChunksChecked == 1);
    assert(healthy.findings.empty());

    const miare::ByteView image{fileView->bytes()};
    for (std::uint64_t offset = miare::detail::commonRegionBytes;
         offset + miare::detail::ExtentLayout::bytes <= image.size();
         offset += miare::DefaultLimits::allocationQuantumBytes) {
        if (miare::detail::matches(
                image, offset, "MIAREXT\0") &&
            miare::detail::readLittleEndian<std::uint16_t>(
                image,
                offset + miare::detail::ExtentLayout::unitKind) == 13) {
            fileView->corruptByte(
                offset + miare::detail::ExtentLayout::nonce);
            break;
        }
    }
    const auto damaged = database.verify();
    assert(!damaged.valid);
    assert(!damaged.findings.empty());
    assert(damaged.findings.front().code ==
        miare::VerificationFindingCode::ExtentAuthenticationFailed);
    assert(damaged.findings.size() <= 64);
    assert(database.state() == miare::DatabaseState::RecoveryRequired);
    database.close();
}

void backupIsPortableAndExcludesAbandonedTail() {
    TemporaryDirectory temporary;
    const auto backupPath = temporary.path() / "backup.miare";
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto* fileView = file.get();
    auto database = miare::testing::DatabaseAccess::create(
        std::move(file),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(3));
    auto write = database.beginWrite();
    write.put(bytes("key"), bytes("portable"));
    write.commit();
    const auto committedBytes = database.diagnostics().mainFileBytes;
    fileView->writeExactAt(committedBytes, bytes("abandoned"));

    const auto report = database.backupTo(backupPath);
    assert(report.sourceGeneration == 2);
    assert(report.destinationFileBytes == committedBytes);
    assert(report.excludedAbandonedTailBytes == 9);
    assert(std::filesystem::file_size(backupPath) == committedBytes);
    assert(database.state() == miare::DatabaseState::Open);

    auto opened = miare::Database<>::open(
        backupPath,
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(4));
    assert(opened);
    auto read = opened.value().beginRead();
    const auto value = read.get(bytes("key"));
    assert(value && std::equal(
        value->begin(), value->end(),
        bytes("portable").begin(), bytes("portable").end()));
    read.end();
    opened.value().close();

    const auto verified = miare::Database<>::verifyFile(
        backupPath,
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(5));
    assert(verified && verified.value().valid);
    const auto truncatedPath = temporary.path() / "truncated.miare";
    assert(std::filesystem::copy_file(backupPath, truncatedPath));
    {
        auto truncated =
            miare::detail::NativeDurableFile::openExisting(truncatedPath);
        truncated->resize(miare::detail::commonRegionBytes);
        truncated->stableStorageBarrier();
    }
    const auto truncated = miare::Database<>::verifyFile(
        truncatedPath,
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(13));
    assert(truncated && !truncated.value().valid);
    assert(truncated.value().findings.front().code ==
        miare::VerificationFindingCode::FileTruncated);
    auto wrongKey = encryptionKey;
    wrongKey.front() ^= std::byte{1};
    const auto rejected = miare::Database<>::verifyFile(
        backupPath,
        miare::EncryptionKeyView{wrongKey},
        deterministicProviders(6));
    assert(!rejected);

    try {
        (void)database.backupTo(backupPath);
        assert(false);
    } catch (const miare::DatabaseError& error) {
        assert(error.code() == miare::Errc::Io);
    }
    assert(database.state() == miare::DatabaseState::Open);
    database.close();
}

} // namespace

int main() {
    checkpointRespectsSnapshotsAndPersistsReclamation();
    verificationIsBoundedAndFailsClosed();
    backupIsPortableAndExcludesAbandonedTail();
}
