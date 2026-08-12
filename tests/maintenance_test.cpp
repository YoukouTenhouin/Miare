#include <miare/database.hpp>
#include <miare/testing/fakes.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

namespace {

constexpr std::array<std::byte, 32> encryptionKey{};

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
    assert(damaged.findings.size() <= 64);
    assert(database.state() == miare::DatabaseState::RecoveryRequired);
    database.close();
}

} // namespace

int main() {
    checkpointRespectsSnapshotsAndPersistsReclamation();
    verificationIsBoundedAndFailsClosed();
}
