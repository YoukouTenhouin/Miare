#include <miare/database.hpp>
#include <miare/testing/fakes.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

namespace {

constexpr std::array<std::byte, 32> encryptionKey{};

[[nodiscard]] miare::ProviderSet deterministicProviders(std::uint64_t seed) {
    return miare::detail::ProviderAccess::make(
        std::make_unique<miare::testing::DeterministicCryptoProvider>(seed),
        std::make_unique<miare::testing::FaultInjectingCompressionProvider>());
}

[[nodiscard]] miare::ByteView bytes(const char* text) {
    return {
        reinterpret_cast<const std::byte*>(text),
        std::char_traits<char>::length(text)};
}

using BlobReader = miare::Database<>::BlobReader;
using BlobWriter = miare::Database<>::BlobWriter;

struct SmallChunkLimits : miare::DefaultLimits {
    static constexpr std::uint64_t blobChunkBytes = 64U * 1024U;
};

struct TightBlobLimits : SmallChunkLimits {
    static constexpr std::uint64_t maxBlobBytes = 8;
    static constexpr std::uint64_t maxBlobBytesPerTransaction = 10;
    static constexpr std::uint64_t maxBlobMutationsPerTransaction = 2;
    static constexpr std::uint32_t maxBlobReadersPerTransaction = 1;
    static constexpr std::uint32_t maxOpenBlobWritersPerTransaction = 1;
};

struct FailingAllocatorState {
    std::atomic<bool> failCopies{false};
    std::atomic<bool> failAllocations{false};
};

template<class T>
class FailingAllocator {
public:
    using value_type = T;

    FailingAllocator()
        : state(std::make_shared<FailingAllocatorState>()) {}

    explicit FailingAllocator(std::shared_ptr<FailingAllocatorState> sharedState)
        : state(std::move(sharedState)) {}

    FailingAllocator(const FailingAllocator& other)
        : state(other.state) {
        failCopyIfRequested();
    }

    FailingAllocator(FailingAllocator&& other) noexcept
        : state(other.state) {}

    template<class U>
    FailingAllocator(const FailingAllocator<U>& other)
        : state(other.state) {
        failCopyIfRequested();
    }

    [[nodiscard]] T* allocate(std::size_t count) {
        if (state->failAllocations.load(std::memory_order_relaxed)) {
            throw std::bad_alloc{};
        }
        return std::allocator<T>{}.allocate(count);
    }

    void deallocate(T* allocation, std::size_t count) noexcept {
        std::allocator<T>{}.deallocate(allocation, count);
    }

    template<class U>
    friend class FailingAllocator;

    template<class U>
    friend bool operator==(
        const FailingAllocator& left,
        const FailingAllocator<U>& right) noexcept {
        return left.state == right.state;
    }

    std::shared_ptr<FailingAllocatorState> state;

private:
    void failCopyIfRequested() const {
        if (state->failCopies.load(std::memory_order_relaxed)) {
            throw std::bad_alloc{};
        }
    }
};

using SmallChunkDatabase = miare::Database<
    std::allocator<std::byte>, SmallChunkLimits>;

[[nodiscard]] bool readsAs(BlobReader& reader, const char* expected);

template<class Operation>
void expectContractError(miare::Errc expected, Operation&& operation) {
    try {
        operation();
        assert(false);
    } catch (const miare::ContractError& error) {
        assert(error.code() == expected);
    }
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

static_assert(std::is_move_constructible_v<BlobReader>);
static_assert(!std::is_copy_constructible_v<BlobReader>);
static_assert(!std::is_move_assignable_v<BlobReader>);
static_assert(std::is_move_constructible_v<BlobWriter>);
static_assert(!std::is_copy_constructible_v<BlobWriter>);
static_assert(!std::is_move_assignable_v<BlobWriter>);

void writeTransactionReadsFinishedBlob() {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto database = miare::testing::DatabaseAccess::create(
        std::move(file),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(1));

    auto transaction = database.beginWrite();
    auto writer = transaction.createBlob();
    const auto id = writer.id();
    assert(writer.position() == 0);
    writer.write(bytes("streamed content"));
    assert(writer.position() == 16);
    writer.finish();
    assert(!writer.active());

    auto opened = transaction.openBlob(id);
    assert(opened);
    assert(opened->id() == id);
    assert(opened->size() == 16);
    assert(opened->position() == 0);
    std::array<std::byte, 32> output{};
    assert(opened->read(output) == 16);
    assert(std::equal(
        output.begin(), output.begin() + 16, bytes("streamed content").begin()));
    assert(opened->read(output) == 0);
    opened->close();

    transaction.rollback();
    database.close();
}

void commitPublishesBlobToNewSnapshotsAndReopen() {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto* fileView = file.get();
    auto database = miare::testing::DatabaseAccess::create(
        std::move(file),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(2));

    auto oldSnapshot = database.beginRead();
    auto transaction = database.beginWrite();
    auto writer = transaction.createBlob();
    const auto id = writer.id();
    writer.write(bytes("committed Blob"));
    writer.finish();
    transaction.commit();

    assert(!oldSnapshot.openBlob(id));
    oldSnapshot.end();

    auto current = database.beginRead();
    auto opened = current.openBlob(id);
    assert(opened);
    std::array<std::byte, 32> output{};
    assert(opened->read(output) == 14);
    assert(std::equal(
        output.begin(), output.begin() + 14, bytes("committed Blob").begin()));
    opened->close();
    current.end();

    auto image = fileView->bytes();
    database.close();
    auto reopenedFile = std::make_unique<miare::testing::MemoryDurableFile>();
    reopenedFile->replaceStableBytes(image);
    auto reopened = miare::testing::DatabaseAccess::open(
        std::move(reopenedFile),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(3));
    assert(reopened);
    auto persisted = reopened.value().beginRead();
    auto persistedBlob = persisted.openBlob(id);
    assert(persistedBlob);
    assert(persistedBlob->size() == 14);
    persistedBlob->seek(10);
    assert(persistedBlob->read(output) == 4);
    assert(std::equal(
        output.begin(), output.begin() + 4, bytes("Blob").begin()));
    persistedBlob->close();
    persisted.end();
    reopened.value().close();
}

[[nodiscard]] bool readsAs(BlobReader& reader, const char* expected) {
    std::array<std::byte, 64> output{};
    const auto expectedBytes = bytes(expected);
    return reader.read(output) == expectedBytes.size() &&
        std::equal(
            output.begin(),
            output.begin() + expectedBytes.size(),
            expectedBytes.begin());
}

void replacementPreservesOpenedVersionsAndPublishesAtomically() {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto database = miare::testing::DatabaseAccess::create(
        std::move(file),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(4));
    auto seed = database.beginWrite();
    auto originalWriter = seed.createBlob();
    const auto id = originalWriter.id();
    originalWriter.write(bytes("original"));
    originalWriter.finish();
    seed.commit();

    auto oldSnapshot = database.beginRead();
    auto oldReader = oldSnapshot.openBlob(id);
    assert(oldReader);

    auto replace = database.beginWrite();
    auto replacement = replace.replaceBlob(id);
    assert(replacement);
    replacement->write(bytes("replacement"));
    auto beforeFinish = replace.openBlob(id);
    assert(beforeFinish && readsAs(*beforeFinish, "original"));
    beforeFinish->close();
    replacement->finish();
    auto afterFinish = replace.openBlob(id);
    assert(afterFinish && readsAs(*afterFinish, "replacement"));
    afterFinish->close();
    replace.commit();

    assert(readsAs(*oldReader, "original"));
    oldReader->close();
    oldSnapshot.end();
    auto current = database.beginRead();
    auto currentReader = current.openBlob(id);
    assert(currentReader && readsAs(*currentReader, "replacement"));
    currentReader->close();
    current.end();

    auto rolledBack = database.beginWrite();
    auto discarded = rolledBack.replaceBlob(id);
    assert(discarded);
    discarded->write(bytes("discarded"));
    discarded->finish();
    rolledBack.rollback();
    auto preserved = database.beginRead();
    auto preservedReader = preserved.openBlob(id);
    assert(preservedReader && readsAs(*preservedReader, "replacement"));
    preservedReader->close();
    preserved.end();
    database.close();
}

void multiChunkBlobSupportsSequentialAndRandomAccess() {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto database = miare::testing::DatabaseAccess::create(
        std::move(file),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(5));
    constexpr auto chunkBytes = miare::DefaultLimits::blobChunkBytes;
    std::vector<std::byte> content(2 * chunkBytes + 17);
    for (std::size_t index = 0; index != content.size(); ++index) {
        content[index] = std::byte{static_cast<unsigned char>(
            (index * 131U + index / 251U) & 0xffU)};
    }

    auto write = database.beginWrite();
    auto writer = write.createBlob();
    const auto id = writer.id();
    writer.write(miare::ByteView{content}.first(chunkBytes - 9));
    writer.write(miare::ByteView{content}.subspan(chunkBytes - 9));
    writer.finish();
    write.commit();

    auto read = database.beginRead();
    auto reader = read.openBlob(id);
    assert(reader);
    std::array<std::byte, 40> crossing{};
    reader->seek(chunkBytes - 13);
    assert(reader->read(crossing) == crossing.size());
    assert(std::equal(
        crossing.begin(),
        crossing.end(),
        content.begin() + chunkBytes - 13));
    reader->seek(2 * chunkBytes + 3);
    std::array<std::byte, 32> tail{};
    assert(reader->read(tail) == 14);
    assert(std::equal(
        tail.begin(), tail.begin() + 14, content.end() - 14));
    assert(reader->read(tail) == 0);
    reader->seek(content.size());
    assert(reader->read({}) == 0);
    reader->close();
    read.end();
    database.close();
}

void eraseAndUnfinishedWritersAreTransactional() {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto database = miare::testing::DatabaseAccess::create(
        std::move(file),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(6));
    auto seed = database.beginWrite();
    auto writer = seed.createBlob();
    const auto id = writer.id();
    writer.finish();
    seed.commit();

    auto unfinishedTransaction = database.beginWrite();
    auto unfinished = unfinishedTransaction.replaceBlob(id);
    assert(unfinished);
    unfinished->write(bytes("not final"));
    expectContractError(miare::Errc::InvalidState, [&] {
        (void)unfinishedTransaction.eraseBlob(id);
    });
    expectContractError(miare::Errc::InvalidState, [&] {
        unfinishedTransaction.commit();
    });
    assert(unfinishedTransaction.active());
    unfinished->abort();
    assert(unfinishedTransaction.stats().blobMutations == 0);
    unfinishedTransaction.rollback();

    auto rolledBack = database.beginWrite();
    assert(rolledBack.eraseBlob(id));
    assert(!rolledBack.openBlob(id));
    rolledBack.rollback();
    auto afterRollback = database.beginRead();
    assert(afterRollback.openBlob(id));
    afterRollback.end();

    auto erased = database.beginWrite();
    assert(erased.eraseBlob(id));
    assert(!erased.eraseBlob(id));
    erased.commit();
    auto absent = database.beginRead();
    assert(!absent.openBlob(id));
    absent.end();
    database.close();
}

void persistedChunksHaveCanonicalIndependentFraming() {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto* fileView = file.get();
    auto database = miare::testing::DatabaseAccess::create(
        std::move(file),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(7));
    constexpr auto chunkBytes = miare::DefaultLimits::blobChunkBytes;
    std::vector<std::byte> content(2 * chunkBytes + 17, std::byte{0x5a});
    auto transaction = database.beginWrite();
    auto writer = transaction.createBlob();
    const auto id = writer.id();
    writer.write(content);
    writer.finish();
    transaction.commit();

    const auto image = fileView->bytes();
    const auto owner = id.toBytes();
    std::uint64_t offset = miare::detail::commonRegionBytes;
    std::uint64_t chunkOrdinal = 0;
    bool sawManifest = false;
    while (offset != image.size()) {
        const miare::ByteView input{image};
        assert(miare::detail::matches(
            input,
            offset + miare::detail::ExtentLayout::magic,
            "MIAREXT\0"));
        const auto kind = miare::detail::readLittleEndian<std::uint16_t>(
            input, offset + miare::detail::ExtentLayout::unitKind);
        const auto blocks = miare::detail::readLittleEndian<std::uint64_t>(
            input, offset + miare::detail::ExtentLayout::blockCount);
        if (kind == 13) {
            assert(miare::detail::readLittleEndian<std::uint32_t>(
                input,
                offset + miare::detail::ExtentLayout::keyDomain) == 4);
            assert(miare::detail::readLittleEndian<std::uint64_t>(
                input,
                offset + miare::detail::ExtentLayout::sequence) ==
                chunkOrdinal);
            assert(std::equal(
                owner.begin(),
                owner.end(),
                input.begin() + offset + miare::detail::ExtentLayout::owner));
            const auto decodedLength =
                miare::detail::readLittleEndian<std::uint64_t>(
                    input,
                    offset + miare::detail::ExtentLayout::decodedLength);
            assert(decodedLength ==
                (chunkOrdinal == 2 ? 17 : chunkBytes));
            const auto flags = miare::detail::readLittleEndian<std::uint32_t>(
                input, offset + miare::detail::ExtentLayout::flags);
            assert(flags == (chunkOrdinal == 2 ? 0U : 1U));
            ++chunkOrdinal;
        } else if (kind == 12) {
            sawManifest = true;
            assert(miare::detail::readLittleEndian<std::uint32_t>(
                input, offset + miare::detail::ExtentLayout::flags) == 0);
            assert(miare::detail::readLittleEndian<std::uint32_t>(
                input,
                offset + miare::detail::ExtentLayout::keyDomain) == 4);
        }
        assert(blocks != 0);
        offset += blocks * miare::DefaultLimits::allocationQuantumBytes;
        assert(offset <= image.size());
    }
    assert(chunkOrdinal == 3);
    assert(sawManifest);
    database.close();
}

void fixedTreeRejectsNonPageExtentKindsBeforeParsing() {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto* fileView = file.get();
    auto database = miare::testing::DatabaseAccess::create(
        std::move(file),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(20));
    auto transaction = database.beginWrite();
    auto writer = transaction.createBlob();
    const auto id = writer.id();
    writer.write(bytes("x"));
    writer.finish();
    transaction.commit();
    const auto image = fileView->bytes();
    database.close();

    constexpr auto quantum = miare::DefaultLimits::allocationQuantumBytes;
    std::optional<miare::detail::ExtentReference> chunk;
    for (std::uint64_t offset = miare::detail::commonRegionBytes;
         offset < image.size();) {
        const miare::ByteView input{image};
        const auto kind = miare::detail::readLittleEndian<std::uint16_t>(
            input, offset + miare::detail::ExtentLayout::unitKind);
        const auto blockCount = miare::detail::readLittleEndian<std::uint64_t>(
            input, offset + miare::detail::ExtentLayout::blockCount);
        if (kind == 13) {
            chunk = miare::detail::ExtentReference{
                offset / quantum,
                blockCount,
                miare::detail::readLittleEndian<std::uint64_t>(
                    input, offset + miare::detail::ExtentLayout::encodedLength),
                miare::detail::readLittleEndian<std::uint64_t>(
                    input, offset + miare::detail::ExtentLayout::generation)};
            break;
        }
        offset += blockCount * quantum;
    }
    assert(chunk);

    miare::testing::MemoryDurableFile persisted;
    persisted.replaceStableBytes(image);
    auto providers = deterministicProviders(21);
    auto openedResult = miare::detail::openFormat<miare::DefaultLimits>(
        persisted,
        miare::EncryptionKeyView{encryptionKey},
        providers);
    assert(openedResult);
    auto opened = std::move(openedResult).value();
    const auto owner = id.toBytes();
    expectDatabaseError(miare::Errc::Corrupt, [&] {
        (void)miare::detail::loadFixedTree<miare::DefaultLimits>(
            persisted,
            *chunk,
            2,
            miare::BlobId::encodedSize,
            3,
            4,
            4,
            owner,
            opened,
            providers,
            std::allocator<std::byte>{});
    });

    using Entry = miare::detail::FixedLeafEntry<std::allocator<std::byte>>;
    miare::detail::StoredVector<Entry, std::allocator<std::byte>> entries;
    expectDatabaseError(miare::Errc::Corrupt, [&] {
        (void)miare::detail::loadFixedTreePage<miare::DefaultLimits>(
            persisted,
            *chunk,
            0,
            2,
            miare::BlobId::encodedSize,
            3,
            4,
            4,
            owner,
            opened,
            providers,
            std::allocator<std::byte>{},
            entries,
            nullptr);
    });
}

void valueAndBlobIdentifierCommitTogether() {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto database = miare::testing::DatabaseAccess::create(
        std::move(file),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(8));
    auto transaction = database.beginWrite();
    auto writer = transaction.createBlob();
    const auto id = writer.id();
    const auto encodedId = id.toBytes();
    transaction.put(bytes("blob-id"), encodedId);
    writer.write(bytes("linked content"));
    writer.finish();
    transaction.commit();

    auto read = database.beginRead();
    const auto storedId = read.get(bytes("blob-id"));
    assert(storedId && storedId->size() == miare::BlobId::encodedSize);
    std::array<std::byte, miare::BlobId::encodedSize> storedIdBytes{};
    std::copy(storedId->begin(), storedId->end(), storedIdBytes.begin());
    const auto decodedId = miare::BlobId::fromBytes(storedIdBytes);
    assert(decodedId == id);
    auto blob = read.openBlob(decodedId);
    assert(blob && readsAs(*blob, "linked content"));
    blob->close();
    read.end();
    database.close();
}

void minimumChunkProfileInteroperatesAcrossReopen() {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto* fileView = file.get();
    auto database = miare::testing::DatabaseAccess::create<
        std::allocator<std::byte>, SmallChunkLimits>(
        std::move(file),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(9));
    std::vector<std::byte> content(
        SmallChunkLimits::blobChunkBytes + 5, std::byte{0x39});
    auto transaction = database.beginWrite();
    auto writer = transaction.createBlob();
    const auto id = writer.id();
    writer.write(content);
    writer.finish();
    transaction.commit();
    auto image = fileView->bytes();
    database.close();

    auto reopenedFile = std::make_unique<miare::testing::MemoryDurableFile>();
    reopenedFile->replaceStableBytes(image);
    auto reopened = miare::testing::DatabaseAccess::open<
        std::allocator<std::byte>, SmallChunkLimits>(
        std::move(reopenedFile),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(10));
    assert(reopened);
    auto read = reopened.value().beginRead();
    auto blob = read.openBlob(id);
    assert(blob && blob->size() == content.size());
    blob->seek(SmallChunkLimits::blobChunkBytes - 2);
    std::array<std::byte, 7> boundary{};
    assert(blob->read(boundary) == boundary.size());
    assert(std::all_of(
        boundary.begin(), boundary.end(),
        [](std::byte value) { return value == std::byte{0x39}; }));
    blob->close();
    read.end();
    reopened.value().close();
}

void tamperedChunkStopsTheSessionBeforePlaintextRelease() {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto* fileView = file.get();
    auto database = miare::testing::DatabaseAccess::create(
        std::move(file),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(11));
    auto transaction = database.beginWrite();
    auto writer = transaction.createBlob();
    const auto id = writer.id();
    writer.write(bytes("authenticated content"));
    writer.finish();
    transaction.commit();

    std::uint64_t offset = miare::detail::commonRegionBytes;
    while (true) {
        const miare::ByteView image{fileView->bytes()};
        const auto kind = miare::detail::readLittleEndian<std::uint16_t>(
            image, offset + miare::detail::ExtentLayout::unitKind);
        if (kind == 13) {
            fileView->corruptByte(
                offset + miare::detail::ExtentLayout::nonce);
            break;
        }
        offset += miare::detail::readLittleEndian<std::uint64_t>(
                      image,
                      offset + miare::detail::ExtentLayout::blockCount) *
            miare::DefaultLimits::allocationQuantumBytes;
    }
    auto read = database.beginRead();
    auto blob = read.openBlob(id);
    assert(blob);
    std::array<std::byte, 32> output{};
    expectDatabaseError(miare::Errc::Corrupt, [&] {
        (void)blob->read(output);
    });
    assert(database.state() == miare::DatabaseState::RecoveryRequired);
    assert(!blob->active());
    assert(!read.active());
    blob->close();
    read.end();
    database.close();
}

void blobLimitsFailWithoutAdvancingStreamState() {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto database = miare::testing::DatabaseAccess::create<
        std::allocator<std::byte>, TightBlobLimits>(
        std::move(file),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(12));
    auto transaction = database.beginWrite();
    auto writer = transaction.createBlob();
    expectDatabaseError(miare::Errc::ResourceLimit, [&] {
        (void)transaction.createBlob();
    });
    writer.write(bytes("12345678"));
    expectDatabaseError(miare::Errc::ResourceLimit, [&] {
        writer.write(bytes("9"));
    });
    assert(writer.position() == 8);
    const auto id = writer.id();
    writer.finish();
    const auto statistics = transaction.stats();
    assert(statistics.blobMutations == 1);
    assert(statistics.blobBytesWritten == 8);
    assert(statistics.estimatedFileGrowthBytes >= 8);
    transaction.commit();

    auto read = database.beginRead();
    auto first = read.openBlob(id);
    assert(first);
    expectDatabaseError(miare::Errc::ResourceLimit, [&] {
        (void)read.openBlob(id);
    });
    expectContractError(miare::Errc::InvalidArgument, [&] {
        first->seek(9);
    });
    assert(first->position() == 0);
    first->close();
    read.end();
    database.close();
}

void failedCommitPublishesNeitherValueNorBlob() {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto* fileView = file.get();
    auto database = miare::testing::DatabaseAccess::create(
        std::move(file),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(13));
    auto seed = database.beginWrite();
    auto seedWriter = seed.createBlob();
    const auto id = seedWriter.id();
    seedWriter.write(bytes("before"));
    seedWriter.finish();
    seed.put(bytes("state"), bytes("before"));
    seed.commit();

    auto transaction = database.beginWrite();
    auto replacement = transaction.replaceBlob(id);
    assert(replacement);
    replacement->write(bytes("after"));
    replacement->finish();
    transaction.put(bytes("state"), bytes("after"));
    fileView->failNextResize();
    expectDatabaseError(miare::Errc::CommitFailed, [&] {
        transaction.commit();
    });
    assert(!transaction.active());
    assert(database.state() == miare::DatabaseState::RecoveryRequired);
    fileView->simulateCrash();
    auto crashImage = fileView->bytes();
    database.close();

    auto reopenedFile = std::make_unique<miare::testing::MemoryDurableFile>();
    reopenedFile->replaceStableBytes(crashImage);
    auto reopened = miare::testing::DatabaseAccess::open(
        std::move(reopenedFile),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(14));
    assert(reopened);
    auto read = reopened.value().beginRead();
    const auto value = read.get(bytes("state"));
    assert(value && std::equal(
        value->begin(), value->end(), bytes("before").begin()));
    auto blob = read.openBlob(id);
    assert(blob && readsAs(*blob, "before"));
    blob->close();
    read.end();
    reopened.value().close();
}

void stagingIoFailurePreservesWriterContents() {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto* fileView = file.get();
    auto database = miare::testing::DatabaseAccess::create<
        std::allocator<std::byte>, SmallChunkLimits>(
        std::move(file),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(15));
    std::vector<std::byte> content(SmallChunkLimits::blobChunkBytes);
    for (std::size_t index = 0; index != content.size(); ++index) {
        content[index] = std::byte{static_cast<unsigned char>(index & 0xffU)};
    }

    auto transaction = database.beginWrite();
    auto writer = transaction.createBlob();
    const auto id = writer.id();
    fileView->failNextResize();
    expectDatabaseError(miare::Errc::Io, [&] {
        writer.write(content);
    });
    assert(writer.position() == 0);
    assert(transaction.stats().blobBytesWritten == 0);
    writer.write(content);
    assert(writer.position() == content.size());
    writer.finish();
    transaction.commit();

    auto read = database.beginRead();
    auto reader = read.openBlob(id);
    assert(reader);
    std::vector<std::byte> output(content.size());
    assert(reader->read(output) == output.size());
    assert(output == content);
    reader->close();
    read.end();
    database.close();
}

void stagingProviderFailurePreservesWriterContents() {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto compression =
        std::make_unique<miare::testing::FaultInjectingCompressionProvider>();
    auto* compressionView = compression.get();
    auto providers = miare::detail::ProviderAccess::make(
        std::make_unique<miare::testing::DeterministicCryptoProvider>(16),
        std::move(compression));
    auto database = miare::testing::DatabaseAccess::create<
        std::allocator<std::byte>, SmallChunkLimits>(
        std::move(file),
        miare::EncryptionKeyView{encryptionKey},
        std::move(providers));
    std::vector<std::byte> content(
        SmallChunkLimits::blobChunkBytes, std::byte{0x5a});

    auto transaction = database.beginWrite();
    auto writer = transaction.createBlob();
    const auto id = writer.id();
    compressionView->failNextProviderOperation();
    expectDatabaseError(miare::Errc::ProviderUnavailable, [&] {
        writer.write(content);
    });
    assert(writer.position() == 0);
    assert(transaction.stats().blobBytesWritten == 0);
    writer.write(content);
    writer.finish();
    transaction.commit();

    auto read = database.beginRead();
    auto reader = read.openBlob(id);
    assert(reader);
    std::vector<std::byte> output(content.size());
    assert(reader->read(output) == output.size());
    assert(output == content);
    reader->close();
    read.end();
    database.close();
}

void stagingAllocatorCorruptionMakesChildrenTerminal() {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto* fileView = file.get();
    auto database = miare::testing::DatabaseAccess::create<
        std::allocator<std::byte>, SmallChunkLimits>(
        std::move(file),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(17));
    auto seed = database.beginWrite();
    seed.put(bytes("seed"), bytes("value"));
    seed.commit();

    const miare::ByteView image{fileView->bytes()};
    bool corrupted = false;
    for (std::uint64_t offset = miare::detail::commonRegionBytes;
         offset + miare::detail::ExtentLayout::bytes <= image.size();
         offset += SmallChunkLimits::allocationQuantumBytes) {
        if (std::equal(
                bytes("MIAREXT").begin(),
                bytes("MIAREXT").end(),
                image.begin() + static_cast<std::ptrdiff_t>(offset)) &&
            miare::detail::readLittleEndian<std::uint16_t>(
                image,
                offset + miare::detail::ExtentLayout::unitKind) == 14) {
            fileView->corruptByte(
                offset + miare::detail::ExtentLayout::nonce);
            corrupted = true;
            break;
        }
    }
    assert(corrupted);

    auto transaction = database.beginWrite();
    auto writer = transaction.createBlob();
    std::vector<std::byte> content(
        SmallChunkLimits::blobChunkBytes, std::byte{0x29});
    expectDatabaseError(miare::Errc::Corrupt, [&] {
        writer.write(content);
    });
    assert(database.state() == miare::DatabaseState::RecoveryRequired);
    assert(!writer.active());
    assert(!transaction.active());
    database.close();
}

void writerConstructionFailurePreservesTransactionContents() {
    auto state = std::make_shared<FailingAllocatorState>();
    FailingAllocator<std::byte> allocator{state};
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto database = miare::testing::DatabaseAccess::create<
        FailingAllocator<std::byte>, SmallChunkLimits>(
        std::move(file),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(18),
        {},
        allocator);

    auto transaction = database.beginWrite();
    state->failCopies.store(true, std::memory_order_relaxed);
    try {
        (void)transaction.createBlob();
        assert(false);
    } catch (const std::bad_alloc&) {
    }
    state->failCopies.store(false, std::memory_order_relaxed);
    assert(transaction.stats().blobMutations == 0);
    assert(transaction.stats().openBlobWriters == 0);
    auto writer = transaction.createBlob();
    const auto id = writer.id();
    writer.finish();
    transaction.commit();

    auto replacementTransaction = database.beginWrite();
    state->failCopies.store(true, std::memory_order_relaxed);
    try {
        (void)replacementTransaction.replaceBlob(id);
        assert(false);
    } catch (const std::bad_alloc&) {
    }
    state->failCopies.store(false, std::memory_order_relaxed);
    assert(replacementTransaction.stats().blobMutations == 0);
    assert(replacementTransaction.stats().openBlobWriters == 0);
    auto replacement = replacementTransaction.replaceBlob(id);
    assert(replacement);
    replacement->abort();
    replacementTransaction.rollback();
    database.close();
}

void abortUsesPreallocatedStagingBookkeeping() {
    auto state = std::make_shared<FailingAllocatorState>();
    FailingAllocator<std::byte> allocator{state};
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto database = miare::testing::DatabaseAccess::create<
        FailingAllocator<std::byte>, SmallChunkLimits>(
        std::move(file),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(19),
        {},
        allocator);

    auto transaction = database.beginWrite();
    auto writer = transaction.createBlob();
    std::vector<std::byte> chunk(
        SmallChunkLimits::blobChunkBytes, std::byte{0x41});
    writer.write(chunk);
    state->failAllocations.store(true, std::memory_order_relaxed);
    writer.abort();
    assert(transaction.stats().blobMutations == 0);
    assert(transaction.stats().openBlobWriters == 0);
    state->failAllocations.store(false, std::memory_order_relaxed);
    transaction.commit();
    database.close();
}

} // namespace

int main() {
    writeTransactionReadsFinishedBlob();
    commitPublishesBlobToNewSnapshotsAndReopen();
    replacementPreservesOpenedVersionsAndPublishesAtomically();
    multiChunkBlobSupportsSequentialAndRandomAccess();
    eraseAndUnfinishedWritersAreTransactional();
    persistedChunksHaveCanonicalIndependentFraming();
    fixedTreeRejectsNonPageExtentKindsBeforeParsing();
    valueAndBlobIdentifierCommitTogether();
    minimumChunkProfileInteroperatesAcrossReopen();
    tamperedChunkStopsTheSessionBeforePlaintextRelease();
    blobLimitsFailWithoutAdvancingStreamState();
    failedCommitPublishesNeitherValueNorBlob();
    stagingIoFailurePreservesWriterContents();
    stagingProviderFailurePreservesWriterContents();
    stagingAllocatorCorruptionMakesChildrenTerminal();
    writerConstructionFailurePreservesTransactionContents();
    abortUsesPreallocatedStagingBookkeeping();
}
