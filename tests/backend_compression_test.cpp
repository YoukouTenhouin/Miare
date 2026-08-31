#include <miare/database.hpp>
#include <miare/testing/fakes.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace {

constexpr std::array<std::byte, 32> encryptionKey{};

[[nodiscard]] miare::ProviderSet deterministicProviders(
    std::uint64_t seed,
    bool withCompression = true) {
    std::unique_ptr<miare::detail::CompressionProvider> compression;
    if (withCompression) {
        compression =
            std::make_unique<miare::testing::FaultInjectingCompressionProvider>();
    }
    return miare::detail::ProviderAccess::make(
        std::make_unique<miare::testing::DeterministicCryptoProvider>(seed),
        std::move(compression));
}

[[nodiscard]] std::vector<std::byte> incompressibleBytes(std::size_t size) {
    std::vector<std::byte> bytes(size);
    std::uint64_t state = 0x243f6a8885a308d3ULL;
    for (auto& byte : bytes) {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        byte = std::byte{static_cast<unsigned char>(state)};
    }
    return bytes;
}

struct ObservedExtentMetadata {
    std::uint16_t kind;
    std::uint32_t flags;
    std::uint64_t blockCount;
    std::uint64_t decodedLength;
};

[[nodiscard]] std::vector<ObservedExtentMetadata> readExtentMetadata(
    const std::vector<std::byte>& image) {
    constexpr auto quantum = miare::DefaultLimits::allocationQuantumBytes;
    std::vector<ObservedExtentMetadata> metadata;
    std::uint64_t offset = miare::detail::commonRegionBytes;
    while (offset != image.size()) {
        const miare::ByteView input{image};
        assert(miare::detail::matches(
            input, offset + miare::detail::ExtentLayout::magic, "MIAREXT\0"));
        const auto blockCount = miare::detail::readLittleEndian<std::uint64_t>(
            input, offset + miare::detail::ExtentLayout::blockCount);
        assert(blockCount != 0);
        metadata.push_back(ObservedExtentMetadata{
            miare::detail::readLittleEndian<std::uint16_t>(
                input, offset + miare::detail::ExtentLayout::unitKind),
            miare::detail::readLittleEndian<std::uint32_t>(
                input, offset + miare::detail::ExtentLayout::flags),
            blockCount,
            miare::detail::readLittleEndian<std::uint64_t>(
                input, offset + miare::detail::ExtentLayout::decodedLength)});
        offset += blockCount * quantum;
        assert(offset <= image.size());
    }
    return metadata;
}

[[nodiscard]] std::vector<std::byte> compressedImage() {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto* fileView = file.get();
    auto database = miare::testing::DatabaseAccess::create(
        std::move(file),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(8));
    auto write = database.beginWrite();
    const std::array key{std::byte{0x40}};
    const std::vector<std::byte> value(8'192, std::byte{0x48});
    write.put(key, value);
    write.commit();
    auto image = fileView->bytes();
    database.close();
    return image;
}

template<class MutatePreamble, class MutateStored>
[[nodiscard]] std::vector<std::byte> rewriteRootExtent(
    const std::vector<std::byte>& image,
    MutatePreamble&& mutatePreamble,
    MutateStored&& mutateStored) {
    constexpr auto quantum = miare::DefaultLimits::allocationQuantumBytes;
    miare::testing::MemoryDurableFile file;
    file.replaceStableBytes(image);
    auto providers = deterministicProviders(9);
    auto openedResult = miare::detail::openFormat<miare::DefaultLimits>(
        file,
        miare::EncryptionKeyView{encryptionKey},
        providers);
    assert(openedResult);
    auto opened = std::move(openedResult).value();
    const auto reference = miare::detail::decodeExtentReference(
        opened.format.orderedRoot);
    std::vector<std::byte> extent(reference.blockCount * quantum);
    file.readExactAt(reference.blockIndex * quantum, extent);
    const auto storedLength = miare::detail::readLittleEndian<std::uint64_t>(
        extent, miare::detail::ExtentLayout::storedLength);
    assert(miare::detail::readLittleEndian<std::uint32_t>(
        extent, miare::detail::ExtentLayout::flags) == 1);
    std::vector<std::byte> stored(storedLength);
    const auto originalAssociatedData = miare::detail::extentAssociatedData(
        opened,
        miare::ByteView{extent}.first(miare::detail::ExtentLayout::bytes));
    auto& crypto = miare::detail::ProviderAccess::crypto(providers);
    assert(crypto.decryptDetached(
        opened.keys->mainData.view(),
        miare::ByteView{extent}.subspan(
            miare::detail::ExtentLayout::nonce,
            miare::detail::aeadNonceBytes),
        miare::ByteView{extent}.subspan(
            miare::detail::ExtentLayout::bytes,
            storedLength),
        miare::ByteView{extent}.subspan(
            miare::detail::ExtentLayout::bytes + storedLength,
            miare::detail::authenticationTagBytes),
        originalAssociatedData,
        stored));
    mutatePreamble(miare::MutableByteView{extent});
    mutateStored(stored);
    const auto associatedData = miare::detail::extentAssociatedData(
        opened,
        miare::ByteView{extent}.first(miare::detail::ExtentLayout::bytes));
    crypto.encryptDetached(
        opened.keys->mainData.view(),
        miare::ByteView{extent}.subspan(
            miare::detail::ExtentLayout::nonce,
            miare::detail::aeadNonceBytes),
        stored,
        associatedData,
        miare::MutableByteView{extent}.subspan(
            miare::detail::ExtentLayout::bytes,
            storedLength),
        miare::MutableByteView{extent}.subspan(
            miare::detail::ExtentLayout::bytes + storedLength,
            miare::detail::authenticationTagBytes));
    auto rewritten = image;
    std::copy(
        extent.begin(),
        extent.end(),
        rewritten.begin() + reference.blockIndex * quantum);
    return rewritten;
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

template<class Providers>
void expectOpenDatabaseError(
    const std::vector<std::byte>& image,
    miare::Errc expected,
    Providers providers) {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    file->replaceStableBytes(image);
    expectDatabaseError(expected, [&] {
        (void)miare::testing::DatabaseAccess::open(
            std::move(file),
            miare::EncryptionKeyView{encryptionKey},
            std::move(providers));
    });
}

[[nodiscard]] std::vector<std::byte> tamperRootNonce(
    const std::vector<std::byte>& image) {
    constexpr auto quantum = miare::DefaultLimits::allocationQuantumBytes;
    miare::testing::MemoryDurableFile file;
    file.replaceStableBytes(image);
    auto providers = deterministicProviders(10);
    auto openedResult = miare::detail::openFormat<miare::DefaultLimits>(
        file,
        miare::EncryptionKeyView{encryptionKey},
        providers);
    assert(openedResult);
    const auto reference = miare::detail::decodeExtentReference(
        openedResult.value().format.orderedRoot);
    auto tampered = image;
    tampered[reference.blockIndex * quantum +
        miare::detail::ExtentLayout::nonce] ^= std::byte{1};
    return tampered;
}

[[nodiscard]] std::vector<std::byte> incompatibleCodecProfile(
    const std::vector<std::byte>& image) {
    miare::testing::MemoryDurableFile file;
    file.replaceStableBytes(image);
    auto providers = deterministicProviders(11);
    auto openedResult = miare::detail::openFormat<miare::DefaultLimits>(
        file,
        miare::EncryptionKeyView{encryptionKey},
        providers);
    assert(openedResult);
    auto opened = std::move(openedResult).value();
    auto publication = opened.publication;
    miare::detail::writeLittleEndian<std::uint32_t>(
        2,
        publication,
        miare::detail::PublicationLayout::codecProfile);
    const auto slotIndex = static_cast<std::uint16_t>(
        opened.format.generation % 2);
    const auto slot = miare::detail::encodePublicationSlot(
        opened,
        publication,
        slotIndex,
        providers);
    auto rewritten = image;
    std::copy(
        slot.begin(),
        slot.end(),
        rewritten.begin() + miare::detail::bootstrapBytes +
            slotIndex * miare::detail::publicationSlotBytes);
    return rewritten;
}

void malformedProviderOutputDoesNotChangeCommittedState() {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto* fileView = file.get();
    auto compression =
        std::make_unique<miare::testing::FaultInjectingCompressionProvider>();
    auto* compressionView = compression.get();
    auto providers = miare::detail::ProviderAccess::make(
        std::make_unique<miare::testing::DeterministicCryptoProvider>(1),
        std::move(compression));
    auto database = miare::testing::DatabaseAccess::create(
        std::move(file),
        miare::EncryptionKeyView{encryptionKey},
        std::move(providers));
    const auto committedBytes = fileView->bytes();

    auto write = database.beginWrite();
    const std::array key{std::byte{0x01}};
    const std::vector<std::byte> value(8'192, std::byte{0x5a});
    write.put(key, value);
    compressionView->corruptNextFrame();
    expectDatabaseError(miare::Errc::ProviderUnavailable, [&] {
        write.commit();
    });

    assert(fileView->bytes() == committedBytes);
    write.rollback();
    auto read = database.beginRead();
    assert(!read.get(key));
    read.end();
    database.close();
}

void excessiveProviderBoundIsRejectedBeforeAllocation() {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto* fileView = file.get();
    auto compression =
        std::make_unique<miare::testing::FaultInjectingCompressionProvider>();
    auto* compressionView = compression.get();
    compressionView->requestMaximumOutputStorage();
    auto providers = miare::detail::ProviderAccess::make(
        std::make_unique<miare::testing::DeterministicCryptoProvider>(6),
        std::move(compression));
    auto database = miare::testing::DatabaseAccess::create(
        std::move(file),
        miare::EncryptionKeyView{encryptionKey},
        std::move(providers));
    const auto committedBytes = fileView->bytes();
    auto write = database.beginWrite();
    const std::array key{std::byte{0x02}};
    const std::vector<std::byte> value(8'192, std::byte{0x6b});
    write.put(key, value);
    expectDatabaseError(miare::Errc::ProviderUnavailable, [&] {
        write.commit();
    });
    assert(fileView->bytes() == committedBytes);
    write.rollback();
    database.close();
}

void excessiveProviderResultIsRejectedBeforeUse() {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto* fileView = file.get();
    auto compression =
        std::make_unique<miare::testing::FaultInjectingCompressionProvider>();
    auto* compressionView = compression.get();
    compressionView->reportExcessiveOutput();
    auto providers = miare::detail::ProviderAccess::make(
        std::make_unique<miare::testing::DeterministicCryptoProvider>(7),
        std::move(compression));
    auto database = miare::testing::DatabaseAccess::create(
        std::move(file),
        miare::EncryptionKeyView{encryptionKey},
        std::move(providers));
    const auto committedBytes = fileView->bytes();
    auto write = database.beginWrite();
    const std::array key{std::byte{0x03}};
    const std::vector<std::byte> value(8'192, std::byte{0x7c});
    write.put(key, value);
    expectDatabaseError(miare::Errc::ProviderUnavailable, [&] {
        write.commit();
    });
    assert(fileView->bytes() == committedBytes);
    write.rollback();
    database.close();
}

void disabledCompressionNeedsNoProviderForBtreeData() {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto* fileView = file.get();
    miare::CreateOptions options;
    options.compression = miare::Compression::None;
    auto database = miare::testing::DatabaseAccess::create(
        std::move(file),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(2, false),
        options);
    const std::array key{std::byte{0x10}};
    const std::vector<std::byte> value(8'192, std::byte{0x37});
    auto write = database.beginWrite();
    write.put(key, value);
    write.commit();
    const auto image = fileView->bytes();
    for (const auto& fact : readExtentMetadata(image)) {
        assert(fact.flags == 0);
    }
    database.close();

    auto reopenedFile = std::make_unique<miare::testing::MemoryDurableFile>();
    reopenedFile->replaceStableBytes(image);
    auto reopenedResult = miare::testing::DatabaseAccess::open(
        std::move(reopenedFile),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(3, false));
    assert(reopenedResult.hasValue());
    auto reopened = std::move(reopenedResult).value();
    auto read = reopened.beginRead();
    const auto reopenedValue = read.get(key);
    assert(reopenedValue);
    assert(*reopenedValue == value);
    read.end();
    reopened.close();
}

void mixedRepresentationsHaveIdenticalPublicResults() {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto* fileView = file.get();
    auto database = miare::testing::DatabaseAccess::create(
        std::move(file),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(4));
    const std::array compressibleKey{std::byte{0x20}};
    const std::array uncompressedKey{std::byte{0x30}};
    const std::vector<std::byte> compressible(8'192, std::byte{0x5a});
    const auto uncompressed = incompressibleBytes(8'192);
    auto write = database.beginWrite();
    write.put(compressibleKey, compressible);
    write.put(uncompressedKey, uncompressed);
    write.commit();
    const auto image = fileView->bytes();

    bool foundCompressedOverflow = false;
    bool foundUncompressedOverflow = false;
    bool foundCompressedPage = false;
    for (const auto& fact : readExtentMetadata(image)) {
        if (fact.kind >= 1 && fact.kind <= 10 && fact.flags == 1) {
            foundCompressedPage = true;
            assert(fact.decodedLength ==
                std::max<std::uint64_t>(
                    16U * 1024U,
                    miare::DefaultLimits::allocationQuantumBytes) -
                    miare::detail::ExtentLayout::bytes -
                    miare::detail::authenticationTagBytes);
        }
        if (fact.kind != 11) {
            continue;
        }
        if (fact.flags == 1) {
            foundCompressedOverflow = true;
            const auto uncompressedBlocks =
                (miare::detail::ExtentLayout::bytes + fact.decodedLength +
                 miare::detail::authenticationTagBytes +
                 miare::DefaultLimits::allocationQuantumBytes - 1) /
                miare::DefaultLimits::allocationQuantumBytes;
            assert(fact.blockCount < uncompressedBlocks);
        } else {
            foundUncompressedOverflow = true;
        }
    }
    assert(foundCompressedPage);
    assert(foundCompressedOverflow);
    assert(foundUncompressedOverflow);
    database.close();

    auto reopenedFile = std::make_unique<miare::testing::MemoryDurableFile>();
    reopenedFile->replaceStableBytes(image);
    auto reopenedResult = miare::testing::DatabaseAccess::open(
        std::move(reopenedFile),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(5));
    assert(reopenedResult.hasValue());
    auto reopened = std::move(reopenedResult).value();
    auto read = reopened.beginRead();
    const auto reopenedCompressible = read.get(compressibleKey);
    assert(reopenedCompressible);
    assert(*reopenedCompressible == compressible);
    const auto reopenedUncompressed = read.get(uncompressedKey);
    assert(reopenedUncompressed);
    assert(*reopenedUncompressed == uncompressed);
    {
        auto cursor = read.scan();
        assert(cursor.first());
        assert(std::equal(
            cursor.key().begin(), cursor.key().end(), compressibleKey.begin()));
        assert(std::equal(
            cursor.value().begin(), cursor.value().end(), compressible.begin()));
        assert(cursor.next());
        assert(std::equal(
            cursor.key().begin(), cursor.key().end(), uncompressedKey.begin()));
        assert(std::equal(
            cursor.value().begin(), cursor.value().end(), uncompressed.begin()));
        assert(!cursor.next());
    }
    read.end();
    reopened.close();
}


void malformedAndTamperedCommittedUnitsFailClosed() {
    const auto image = compressedImage();
    const auto malformedFrame = rewriteRootExtent(
        image,
        [](miare::MutableByteView) {},
        [](std::vector<std::byte>& stored) {
            stored.front() ^= std::byte{1};
        });
    expectOpenDatabaseError(
        malformedFrame,
        miare::Errc::Corrupt,
        deterministicProviders(12));

    const auto tamperedNonce = tamperRootNonce(image);
    expectOpenDatabaseError(
        tamperedNonce,
        miare::Errc::Corrupt,
        deterministicProviders(13));

    const auto contradictoryCodec = rewriteRootExtent(
        image,
        [](miare::MutableByteView extent) {
            miare::detail::writeLittleEndian<std::uint32_t>(
                0,
                extent,
                miare::detail::ExtentLayout::codec);
        },
        [](std::vector<std::byte>&) {});
    expectOpenDatabaseError(
        contradictoryCodec,
        miare::Errc::Corrupt,
        deterministicProviders(18));

    const auto contradictoryProfile = rewriteRootExtent(
        image,
        [](miare::MutableByteView extent) {
            miare::detail::writeLittleEndian<std::uint32_t>(
                0,
                extent,
                miare::detail::ExtentLayout::codecProfile);
        },
        [](std::vector<std::byte>&) {});
    expectOpenDatabaseError(
        contradictoryProfile,
        miare::Errc::Corrupt,
        deterministicProviders(19));

    const auto oversizedDecode = rewriteRootExtent(
        image,
        [](miare::MutableByteView extent) {
            miare::detail::writeLittleEndian<std::uint64_t>(
                std::numeric_limits<std::uint64_t>::max(),
                extent,
                miare::detail::ExtentLayout::decodedLength);
        },
        [](std::vector<std::byte>&) {});
    auto faulting =
        std::make_unique<miare::testing::FaultInjectingCompressionProvider>();
    faulting->failNextProviderOperation();
    auto providers = miare::detail::ProviderAccess::make(
        std::make_unique<miare::testing::DeterministicCryptoProvider>(14),
        std::move(faulting));
    expectOpenDatabaseError(
        oversizedDecode,
        miare::Errc::Corrupt,
        std::move(providers));
}

void profileAndProviderFailuresAreStableAndNonmutating() {
    const auto image = compressedImage();
    expectOpenDatabaseError(
        incompatibleCodecProfile(image),
        miare::Errc::IncompatibleProfile,
        deterministicProviders(15));
    expectOpenDatabaseError(
        image,
        miare::Errc::ProviderUnavailable,
        deterministicProviders(16, false));

    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    file->replaceStableBytes(image);
    auto compression =
        std::make_unique<miare::testing::FaultInjectingCompressionProvider>();
    compression->failNextProviderOperation();
    auto providers = miare::detail::ProviderAccess::make(
        std::make_unique<miare::testing::DeterministicCryptoProvider>(17),
        std::move(compression));
    expectDatabaseError(miare::Errc::ProviderUnavailable, [&] {
        (void)miare::testing::DatabaseAccess::open(
            std::move(file),
            miare::EncryptionKeyView{encryptionKey},
            std::move(providers));
    });
}

} // namespace

int main() {
    malformedProviderOutputDoesNotChangeCommittedState();
    excessiveProviderBoundIsRejectedBeforeAllocation();
    excessiveProviderResultIsRejectedBeforeUse();
    disabledCompressionNeedsNoProviderForBtreeData();
    mixedRepresentationsHaveIdenticalPublicResults();
    malformedAndTamperedCommittedUnitsFailClosed();
    profileAndProviderFailuresAreStableAndNonmutating();
}
