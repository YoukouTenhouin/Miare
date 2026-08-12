#include <miare/database.hpp>
#include <miare/testing/fakes.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::array<std::byte, 32> encryptionKey{};

struct RecoveryLimits : miare::DefaultLimits {
    static constexpr std::uint64_t blobChunkBytes = 64U * 1024U;
};

using RecoveryDatabase = miare::Database<
    std::allocator<std::byte>, RecoveryLimits>;

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

struct Fixture {
    std::vector<std::byte> image;
    miare::BlobId blobId;
};

[[nodiscard]] Fixture predecessorFixture() {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto* fileView = file.get();
    miare::CreateOptions options;
    options.compression = miare::Compression::None;
    auto database = miare::testing::DatabaseAccess::create<
        std::allocator<std::byte>, RecoveryLimits>(
        std::move(file),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(1),
        options);
    auto write = database.beginWrite();
    write.put(bytes("state"), bytes("before"));
    write.put(
        bytes("overflow"),
        std::vector<std::byte>(8'193, std::byte{0x31}));
    auto blob = write.createBlob();
    const auto blobId = blob.id();
    blob.write(bytes("before blob"));
    blob.finish();
    write.commit();
    auto image = fileView->bytes();
    database.close();
    return Fixture{std::move(image), blobId};
}

struct CandidateTrace {
    std::size_t commitOperationStart;
    std::size_t publicationOperation;
    std::vector<miare::testing::DurableFileOperation> operations;
};

[[nodiscard]] CandidateTrace applyCandidate(
    RecoveryDatabase& database,
    miare::testing::MemoryDurableFile& file,
    miare::BlobId blobId) {
    auto write = database.beginWrite();
    auto blob = write.replaceBlob(blobId);
    assert(blob);
    std::vector<std::byte> blobContent(
        RecoveryLimits::blobChunkBytes + 17, std::byte{0x52});
    try {
        blob->write(blobContent);
        blob->finish();
        write.put(bytes("state"), bytes("after"));
        write.put(
            bytes("overflow"),
            std::vector<std::byte>(12'289, std::byte{0x53}));
        for (std::uint16_t index = 0; index != 96; ++index) {
            const auto key = "tree-" + std::to_string(index);
            write.put(bytes(key), std::vector<std::byte>(300, std::byte{0x54}));
        }
        const auto commitOperationStart = file.operations().size();
        write.commit();
        const auto& operations = file.operations();
        const auto publication = std::find_if(
            operations.begin(), operations.end(), [](const auto& operation) {
                return operation.kind ==
                        miare::testing::DurableFileOperationKind::Write &&
                    operation.offset >= miare::detail::bootstrapBytes &&
                    operation.offset < miare::detail::commonRegionBytes;
            });
        assert(publication != operations.end());
        return CandidateTrace{
            commitOperationStart,
            static_cast<std::size_t>(publication - operations.begin()),
            operations};
    } catch (...) {
        if (blob->active()) {
            blob->abort();
        }
        if (write.active()) {
            write.rollback();
        }
        throw;
    }
}

[[nodiscard]] RecoveryDatabase reopen(
    const std::vector<std::byte>& image,
    std::uint64_t providerSeed) {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    file->replaceStableBytes(image);
    auto opened = miare::testing::DatabaseAccess::open<
        std::allocator<std::byte>, RecoveryLimits>(
        std::move(file),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(providerSeed));
    assert(opened);
    return std::move(opened).value();
}

void assertPredecessor(
    const std::vector<std::byte>& image,
    miare::BlobId blobId,
    std::uint64_t providerSeed) {
    auto database = reopen(image, providerSeed);
    auto read = database.beginRead();
    const auto state = read.get(bytes("state"));
    assert(state && std::equal(
        state->begin(), state->end(),
        bytes("before").begin(), bytes("before").end()));
    const auto overflow = read.get(bytes("overflow"));
    assert(overflow && overflow->size() == 8'193);
    assert(!read.contains(bytes("tree-0")));
    auto blob = read.openBlob(blobId);
    assert(blob);
    std::array<std::byte, 11> content{};
    assert(blob->read(content) == content.size());
    assert(std::equal(content.begin(), content.end(), bytes("before blob").begin()));
    blob->close();
    read.end();
    database.close();
}

void assertCandidate(
    const std::vector<std::byte>& image,
    miare::BlobId blobId,
    std::uint64_t providerSeed) {
    auto database = reopen(image, providerSeed);
    auto read = database.beginRead();
    const auto state = read.get(bytes("state"));
    assert(state && std::equal(
        state->begin(), state->end(),
        bytes("after").begin(), bytes("after").end()));
    const auto overflow = read.get(bytes("overflow"));
    assert(overflow && overflow->size() == 12'289);
    assert(read.contains(bytes("tree-0")));
    assert(read.contains(bytes("tree-95")));
    auto blob = read.openBlob(blobId);
    assert(blob);
    std::vector<std::byte> content(
        RecoveryLimits::blobChunkBytes + 17);
    assert(blob->read(content) == content.size());
    assert(std::all_of(content.begin(), content.end(), [](std::byte byte) {
        return byte == std::byte{0x52};
    }));
    blob->close();
    read.end();
    database.close();
}

void everyPersistenceOperationCanBeInterruptedBeforeMutation() {
    const auto fixture = predecessorFixture();
    auto baselineFile = std::make_unique<miare::testing::MemoryDurableFile>();
    auto* baselineFileView = baselineFile.get();
    baselineFile->replaceStableBytes(fixture.image);
    auto baselineResult = miare::testing::DatabaseAccess::open<
        std::allocator<std::byte>, RecoveryLimits>(
        std::move(baselineFile),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(2));
    assert(baselineResult);
    auto baseline = std::move(baselineResult).value();
    baselineFileView->clearOperations();
    const auto trace = applyCandidate(
        baseline, *baselineFileView, fixture.blobId);
    baseline.close();

    std::uint64_t providerSeed = 10;
    for (std::size_t operationIndex = 0;
         operationIndex != trace.operations.size();
         ++operationIndex) {
        const auto operation = trace.operations[operationIndex];
        if (operation.kind == miare::testing::DurableFileOperationKind::Read) {
            continue;
        }
        auto file = std::make_unique<miare::testing::MemoryDurableFile>();
        auto* fileView = file.get();
        file->replaceStableBytes(fixture.image);
        auto opened = miare::testing::DatabaseAccess::open<
            std::allocator<std::byte>, RecoveryLimits>(
            std::move(file),
            miare::EncryptionKeyView{encryptionKey},
            deterministicProviders(providerSeed++));
        assert(opened);
        auto database = std::move(opened).value();
        fileView->clearOperations();
        fileView->failOperation(operationIndex, 0);
        std::optional<miare::Errc> failure;
        try {
            (void)applyCandidate(database, *fileView, fixture.blobId);
        } catch (const miare::DatabaseError& error) {
            failure = error.code();
        }
        assert(failure);
        const auto expected = operationIndex < trace.commitOperationStart
            ? miare::Errc::Io
            : operationIndex < trace.publicationOperation
                ? miare::Errc::CommitFailed
                : miare::Errc::CommitOutcomeUnknown;
        if (*failure != expected) {
            std::fprintf(
                stderr,
                "operation %zu kind %u expected %u actual %u commit %zu publication %zu\n",
                operationIndex,
                static_cast<unsigned>(operation.kind),
                static_cast<unsigned>(expected),
                static_cast<unsigned>(*failure),
                trace.commitOperationStart,
                trace.publicationOperation);
        }
        assert(*failure == expected);
        fileView->simulateCrash();
        const auto crashImage = fileView->bytes();
        assertPredecessor(crashImage, fixture.blobId, providerSeed++);
    }
}

void everyPublicationSectorSubsetSelectsOneCompleteGeneration() {
    const auto fixture = predecessorFixture();
    auto traceFile = std::make_unique<miare::testing::MemoryDurableFile>();
    auto* traceFileView = traceFile.get();
    traceFile->replaceStableBytes(fixture.image);
    auto traceOpened = miare::testing::DatabaseAccess::open<
        std::allocator<std::byte>, RecoveryLimits>(
        std::move(traceFile),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(100));
    assert(traceOpened);
    auto traceDatabase = std::move(traceOpened).value();
    traceFileView->clearOperations();
    const auto trace = applyCandidate(
        traceDatabase, *traceFileView, fixture.blobId);
    traceDatabase.close();

    constexpr std::size_t sectorBytes = 512;
    constexpr std::size_t sectorCount =
        miare::detail::publicationSlotBytes / sectorBytes;
    static_assert(sectorCount == 8);
    std::uint64_t providerSeed = 200;
    for (std::uint16_t retainedMask = 0;
         retainedMask != (1U << sectorCount);
         ++retainedMask) {
        auto file = std::make_unique<miare::testing::MemoryDurableFile>();
        auto* fileView = file.get();
        file->replaceStableBytes(fixture.image);
        auto opened = miare::testing::DatabaseAccess::open<
            std::allocator<std::byte>, RecoveryLimits>(
            std::move(file),
            miare::EncryptionKeyView{encryptionKey},
            deterministicProviders(providerSeed++));
        assert(opened);
        auto database = std::move(opened).value();
        fileView->clearOperations();
        fileView->setMaxTransferBytes(sectorBytes);
        fileView->failOperation(trace.publicationOperation + 1);
        try {
            (void)applyCandidate(database, *fileView, fixture.blobId);
            assert(false);
        } catch (const miare::DatabaseError& error) {
            assert(error.code() == miare::Errc::CommitOutcomeUnknown);
        }
        assert(fileView->unbarrieredMutationCount() == sectorCount);
        std::array<std::size_t, sectorCount> retained{};
        std::size_t retainedCount = 0;
        for (std::size_t sector = 0; sector != sectorCount; ++sector) {
            if ((retainedMask & (1U << sector)) != 0) {
                retained[retainedCount++] = sector;
            }
        }
        fileView->simulateCrash(
            std::span<const std::size_t>{retained}.first(retainedCount));
        const auto crashImage = fileView->bytes();
        if (retainedCount == sectorCount) {
            assertCandidate(crashImage, fixture.blobId, providerSeed++);
        } else {
            assertPredecessor(crashImage, fixture.blobId, providerSeed++);
        }
    }
}

void shortAndTornWritesCannotExposeMixedState() {
    const auto fixture = predecessorFixture();
    auto traceFile = std::make_unique<miare::testing::MemoryDurableFile>();
    auto* traceFileView = traceFile.get();
    traceFile->replaceStableBytes(fixture.image);
    auto traceOpened = miare::testing::DatabaseAccess::open<
        std::allocator<std::byte>, RecoveryLimits>(
        std::move(traceFile),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(800));
    assert(traceOpened);
    auto traceDatabase = std::move(traceOpened).value();
    traceFileView->clearOperations();
    const auto trace = applyCandidate(
        traceDatabase, *traceFileView, fixture.blobId);
    traceDatabase.close();

    std::uint64_t providerSeed = 900;
    for (std::size_t operationIndex = 0;
         operationIndex <= trace.publicationOperation;
         ++operationIndex) {
        const auto& operation = trace.operations[operationIndex];
        if (operation.kind != miare::testing::DurableFileOperationKind::Write ||
            operation.requestedBytes == 0) {
            continue;
        }
        const std::array<std::size_t, 5> candidatePoints{
            0,
            std::min<std::size_t>(1, operation.requestedBytes - 1),
            std::min<std::size_t>(512, operation.requestedBytes - 1),
            std::min<std::size_t>(
                RecoveryLimits::allocationQuantumBytes,
                operation.requestedBytes - 1),
            operation.requestedBytes - 1};
        std::vector<std::size_t> tearPoints{
            candidatePoints.begin(), candidatePoints.end()};
        std::sort(tearPoints.begin(), tearPoints.end());
        tearPoints.erase(
            std::unique(tearPoints.begin(), tearPoints.end()),
            tearPoints.end());
        for (const auto tearPoint : tearPoints) {
            auto file = std::make_unique<miare::testing::MemoryDurableFile>();
            auto* fileView = file.get();
            file->replaceStableBytes(fixture.image);
            auto opened = miare::testing::DatabaseAccess::open<
                std::allocator<std::byte>, RecoveryLimits>(
                std::move(file),
                miare::EncryptionKeyView{encryptionKey},
                deterministicProviders(providerSeed++));
            assert(opened);
            auto database = std::move(opened).value();
            fileView->clearOperations();
            fileView->failOperation(operationIndex, tearPoint);
            try {
                (void)applyCandidate(database, *fileView, fixture.blobId);
                assert(false);
            } catch (const miare::DatabaseError& error) {
                const auto expected =
                    operationIndex < trace.commitOperationStart
                    ? miare::Errc::Io
                    : operationIndex < trace.publicationOperation
                        ? miare::Errc::CommitFailed
                        : miare::Errc::CommitOutcomeUnknown;
                assert(error.code() == expected);
            }
            std::vector<std::size_t> retained(
                fileView->unbarrieredMutationCount());
            for (std::size_t index = 0; index != retained.size(); ++index) {
                retained[index] = index;
            }
            fileView->simulateCrash(retained);
            assertPredecessor(
                fileView->bytes(), fixture.blobId, providerSeed++);
        }
    }
}

} // namespace

int main() {
    everyPersistenceOperationCanBeInterruptedBeforeMutation();
    everyPublicationSectorSubsetSelectsOneCompleteGeneration();
    shortAndTornWritesCannotExposeMixedState();
}
