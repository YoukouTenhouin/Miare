#pragma once

#include <miare/detail/database_format.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace miare::detail {

inline constexpr std::uint32_t maximumTreeLevel = 64;

struct UnsignedBytesLess {
    using is_transparent = void;

    template<class Left, class Right>
    [[nodiscard]] bool operator()(
        const Left& left,
        const Right& right) const noexcept {
        return std::lexicographical_compare(
            left.begin(), left.end(), right.begin(), right.end(),
            [](std::byte lhs, std::byte rhs) {
                return std::to_integer<unsigned char>(lhs) <
                    std::to_integer<unsigned char>(rhs);
            });
    }
};

template<class Allocator>
using StoredBytes = std::vector<
    std::byte,
    typename std::allocator_traits<Allocator>::template rebind_alloc<std::byte>>;

template<class T, class Allocator>
using StoredVector = std::vector<
    T,
    typename std::allocator_traits<Allocator>::template rebind_alloc<T>>;

template<class Allocator>
using StoredKeyValue = std::pair<
    const StoredBytes<Allocator>,
    StoredBytes<Allocator>>;

template<class Allocator>
using OrderedKeyValues = std::map<
    StoredBytes<Allocator>,
    StoredBytes<Allocator>,
    UnsignedBytesLess,
    typename std::allocator_traits<Allocator>::template rebind_alloc<
        StoredKeyValue<Allocator>>>;

template<class Allocator>
[[nodiscard]] inline OrderedKeyValues<Allocator> makeOrderedKeyValues(
    const Allocator& allocator) {
    using MapAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<StoredKeyValue<Allocator>>;
    return OrderedKeyValues<Allocator>{UnsignedBytesLess{}, MapAllocator{allocator}};
}

struct ExtentReference {
    std::uint64_t blockIndex = 0;
    std::uint64_t blockCount = 0;
    std::uint64_t encodedLength = 0;
    std::uint64_t creationGeneration = 0;

    [[nodiscard]] bool null() const noexcept {
        return blockIndex == 0 && blockCount == 0 && encodedLength == 0 &&
            creationGeneration == 0;
    }
};

template<class Allocator>
struct BlobVersion {
    BlobVersion(
        BlobId blobId,
        std::uint64_t blobSize,
        std::uint64_t contentGeneration,
        StoredVector<ExtentReference, Allocator> chunkReferences,
        std::uint64_t stagedHighWater)
        : id(blobId),
          size(blobSize),
          generation(contentGeneration),
          chunks(std::move(chunkReferences)),
          reachable(
              chunks.begin(), chunks.end(), chunks.get_allocator()),
          stagedHighWaterBytes(stagedHighWater),
          pending(true) {}

    BlobVersion(
        BlobId blobId,
        std::uint64_t blobSize,
        std::uint64_t contentGeneration,
        ExtentReference manifestReference,
        ExtentReference chunkRootReference,
        StoredVector<ExtentReference, Allocator> chunkReferences,
        StoredVector<ExtentReference, Allocator> reachableReferences)
        : id(blobId),
          size(blobSize),
          generation(contentGeneration),
          manifest(manifestReference),
          chunkRoot(chunkRootReference),
          chunks(std::move(chunkReferences)),
          reachable(std::move(reachableReferences)) {}

    BlobId id;
    std::uint64_t size;
    std::uint64_t generation = 0;
    ExtentReference manifest;
    ExtentReference chunkRoot;
    StoredVector<ExtentReference, Allocator> chunks;
    StoredVector<ExtentReference, Allocator> reachable;
    std::uint64_t stagedHighWaterBytes = 0;
    bool pending = false;
};

template<class Allocator>
using BlobVersionPtr = std::shared_ptr<const BlobVersion<Allocator>>;

template<class Allocator>
using BlobCatalogEntry = std::pair<const BlobId, BlobVersionPtr<Allocator>>;

template<class Allocator>
using BlobCatalog = std::map<
    BlobId,
    BlobVersionPtr<Allocator>,
    std::less<BlobId>,
    typename std::allocator_traits<Allocator>::template rebind_alloc<
        BlobCatalogEntry<Allocator>>>;

template<class Allocator>
[[nodiscard]] inline BlobCatalog<Allocator> makeBlobCatalog(
    const Allocator& allocator) {
    using Catalog = BlobCatalog<Allocator>;
    return Catalog{
        std::less<BlobId>{},
        typename Catalog::allocator_type{allocator}};
}

template<class Allocator>
struct MutableTreeNode;

struct ActiveReader {
    std::uint64_t generation = 0;
    std::chrono::steady_clock::time_point startedAt;
};

template<class Allocator, class Limits>
struct DatabaseSession {
    DatabaseSession(
        std::unique_ptr<DurableFile> openedFile,
        ProviderSet openedProviders,
        Allocator openedAllocator,
        OpenedDatabase openedDatabase,
        OrderedKeyValues<Allocator> openedValues,
        std::size_t configuredCacheCapacityBytes,
        std::uint32_t configuredMaxReaders)
        : file(std::move(openedFile)),
          providers(std::move(openedProviders)),
          allocator(std::move(openedAllocator)),
          opened(std::move(openedDatabase)),
          values(std::move(openedValues)),
          blobs(makeBlobCatalog(allocator)),
          cursorTree(std::allocate_shared<MutableTreeNode<Allocator>>(
              typename std::allocator_traits<Allocator>::
                  template rebind_alloc<MutableTreeNode<Allocator>>{allocator},
              allocator)),
          activeReaders(
              std::less<std::uint64_t>{},
              typename std::allocator_traits<Allocator>::template rebind_alloc<
                  std::pair<const std::uint64_t, ActiveReader>>{allocator}),
          retiredBlocksByGeneration(
              std::less<std::uint64_t>{},
              typename std::allocator_traits<Allocator>::template rebind_alloc<
                  std::pair<const std::uint64_t, std::uint64_t>>{allocator}),
          cacheCapacityBytes(configuredCacheCapacityBytes),
          maxReaders(configuredMaxReaders) {}

    std::unique_ptr<DurableFile> file;
    std::optional<ProviderSet> providers;
    Allocator allocator;
    OpenedDatabase opened;
    OrderedKeyValues<Allocator> values;
    bool valuesLoaded = false;
    BlobCatalog<Allocator> blobs;
    bool blobsLoaded = false;
    std::shared_ptr<MutableTreeNode<Allocator>> cursorTree;
    std::map<
        std::uint64_t,
        ActiveReader,
        std::less<std::uint64_t>,
        typename std::allocator_traits<Allocator>::template rebind_alloc<
            std::pair<const std::uint64_t, ActiveReader>>> activeReaders;
    std::map<
        std::uint64_t,
        std::uint64_t,
        std::less<std::uint64_t>,
        typename std::allocator_traits<Allocator>::template rebind_alloc<
            std::pair<const std::uint64_t, std::uint64_t>>>
        retiredBlocksByGeneration;
    std::mutex mutex;
    std::condition_variable writerAvailable;
    std::uint64_t nextWriterTicket = 0;
    std::uint64_t servingWriterTicket = 0;
    std::size_t waitingWriters = 0;
    bool writerActive = false;
    std::size_t liveTransactions = 0;
    std::uint64_t nextReaderIdentity = 0;
    std::uint64_t liveBlocks =
        commonRegionBytes / Limits::allocationQuantumBytes;
    bool allocatorSnapshotLoaded = opened.format.generation == 1;
    std::size_t cacheCapacityBytes;
    std::uint32_t maxReaders;
    std::atomic<DatabaseState> state{DatabaseState::Open};
    std::atomic<RecoveryCause> recoveryCause{RecoveryCause::None};
    std::atomic<std::uint64_t> capacityFailureCount{0};
};

template<class Allocator>
struct AllocatorSnapshot {
    using RetirementCounts = std::map<
        std::uint64_t,
        std::uint64_t,
        std::less<std::uint64_t>,
        typename std::allocator_traits<Allocator>::template rebind_alloc<
            std::pair<const std::uint64_t, std::uint64_t>>>;

    std::uint64_t liveBlocks = 0;
    RetirementCounts retiredBlocksByGeneration;
};

struct ExtentLayout {
    static constexpr std::size_t magic = 0;
    static constexpr std::size_t version = 8;
    static constexpr std::size_t unitKind = 10;
    static constexpr std::size_t flags = 12;
    static constexpr std::size_t keyDomain = 16;
    static constexpr std::size_t codec = 20;
    static constexpr std::size_t codecProfile = 24;
    static constexpr std::size_t preambleLength = 28;
    static constexpr std::size_t blockIndex = 32;
    static constexpr std::size_t blockCount = 40;
    static constexpr std::size_t encodedLength = 48;
    static constexpr std::size_t storedLength = 56;
    static constexpr std::size_t decodedLength = 64;
    static constexpr std::size_t generation = 72;
    static constexpr std::size_t sequence = 80;
    static constexpr std::size_t owner = 88;
    static constexpr std::size_t nonce = 104;
    static constexpr std::size_t reserved = 128;
    static constexpr std::size_t bytes = 160;
};

struct PageLayout {
    static constexpr std::size_t magic = 0;
    static constexpr std::size_t version = 8;
    static constexpr std::size_t type = 10;
    static constexpr std::size_t headerLength = 12;
    static constexpr std::size_t role = 16;
    static constexpr std::size_t level = 20;
    static constexpr std::size_t entryCount = 24;
    static constexpr std::size_t prefixLength = 28;
    static constexpr std::size_t slotsOffset = 32;
    static constexpr std::size_t entriesOffset = 36;
    static constexpr std::size_t usedLength = 40;
    static constexpr std::size_t flags = 44;
    static constexpr std::size_t leftmostChild = 48;
    static constexpr std::size_t reserved = 80;
    static constexpr std::size_t bytes = 96;
};

struct AllocatorRootLayout {
    static constexpr std::size_t magic = 0;
    static constexpr std::size_t version = 8;
    static constexpr std::size_t flags = 10;
    static constexpr std::size_t length = 12;
    static constexpr std::size_t generation = 16;
    static constexpr std::size_t highWaterBlocks = 24;
    static constexpr std::size_t freeRoot = 32;
    static constexpr std::size_t retiredRoot = 64;
    static constexpr std::size_t reachableBlocks = 96;
    static constexpr std::size_t freeBlocks = 104;
    static constexpr std::size_t retiredBlocks = 112;
    static constexpr std::size_t reserved = 120;
    static constexpr std::size_t bytes = 160;
};

struct BlobManifestLayout {
    static constexpr std::size_t magic = 0;
    static constexpr std::size_t version = 8;
    static constexpr std::size_t flags = 10;
    static constexpr std::size_t length = 12;
    static constexpr std::size_t id = 16;
    static constexpr std::size_t generation = 32;
    static constexpr std::size_t logicalLength = 40;
    static constexpr std::size_t chunkSize = 48;
    static constexpr std::size_t chunkCount = 56;
    static constexpr std::size_t chunkRoot = 64;
    static constexpr std::size_t reserved = 96;
    static constexpr std::size_t bytes = 128;
};

struct ExtentRun {
    std::uint64_t start;
    std::uint64_t count;
    std::uint64_t retirementGeneration = 0;
};

template<class Allocator>
using ExtentReferences = StoredVector<ExtentReference, Allocator>;

template<class Allocator>
using ExtentRuns = StoredVector<ExtentRun, Allocator>;

template<class Allocator>
using BlobIdSet = std::set<
    BlobId,
    std::less<BlobId>,
    typename std::allocator_traits<Allocator>::template rebind_alloc<BlobId>>;

struct BlobReaderLifetime {
    std::atomic<bool> invalidated{false};
    std::atomic<std::uint32_t> liveReaders{0};
};

template<class Allocator, class Limits>
struct BlobWriteState {
    explicit BlobWriteState(DatabaseSession<Allocator, Limits>& owner)
        : session(&owner),
          blobs(makeBlobCatalog(owner.allocator)),
          generatedIds(
              std::less<BlobId>{},
              typename BlobIdSet<Allocator>::allocator_type{owner.allocator}),
          openWriterIds(
              std::less<BlobId>{},
              typename BlobIdSet<Allocator>::allocator_type{owner.allocator}),
          stagingFreeRuns(
              typename std::allocator_traits<Allocator>::template rebind_alloc<
                  ExtentRun>{owner.allocator}),
          discardedStagedReferences(
              typename std::allocator_traits<Allocator>::template rebind_alloc<
                  ExtentReference>{owner.allocator}),
          thread(std::this_thread::get_id()) {
        blobs = owner.blobs;
    }

    DatabaseSession<Allocator, Limits>* session;
    BlobCatalog<Allocator> blobs;
    BlobIdSet<Allocator> generatedIds;
    BlobIdSet<Allocator> openWriterIds;
    ExtentRuns<Allocator> stagingFreeRuns;
    ExtentReferences<Allocator> discardedStagedReferences;
    std::uint64_t stagingNextBlock = 0;
    std::thread::id thread;
    std::uint64_t mutations = 0;
    std::uint64_t bytesWritten = 0;
    std::size_t abortableStagingReferences = 0;
    std::uint32_t openWriters = 0;
    bool allocatorInitialized = false;
    std::atomic<bool> active{true};
};

[[nodiscard]] inline ExtentReference decodeExtentReference(ByteView bytes) {
    return ExtentReference{
        readLittleEndian<std::uint64_t>(bytes, 0),
        readLittleEndian<std::uint64_t>(bytes, 8),
        readLittleEndian<std::uint64_t>(bytes, 16),
        readLittleEndian<std::uint64_t>(bytes, 24)};
}

[[nodiscard]] inline std::array<std::byte, 32> encodeExtentReference(
    const ExtentReference& reference) {
    std::array<std::byte, 32> bytes{};
    writeLittleEndian<std::uint64_t>(reference.blockIndex, bytes, 0);
    writeLittleEndian<std::uint64_t>(reference.blockCount, bytes, 8);
    writeLittleEndian<std::uint64_t>(reference.encodedLength, bytes, 16);
    writeLittleEndian<std::uint64_t>(reference.creationGeneration, bytes, 24);
    return bytes;
}

template<class Limits>
inline void validateExtentReference(
    const ExtentReference& reference,
    std::uint64_t containingGeneration,
    std::uint64_t highWaterBytes) {
    if (reference.null()) {
        return;
    }
    const auto commonBlocks = commonRegionBytes / Limits::allocationQuantumBytes;
    if (reference.blockIndex < commonBlocks || reference.blockCount == 0 ||
        reference.encodedLength < ExtentLayout::bytes + authenticationTagBytes ||
        reference.creationGeneration == 0 ||
        reference.creationGeneration > containingGeneration ||
        reference.blockCount >
            std::numeric_limits<std::uint64_t>::max() / Limits::allocationQuantumBytes) {
        throwCorrupt("extent reference is invalid");
    }
    const auto allocatedBytes = reference.blockCount * Limits::allocationQuantumBytes;
    if (reference.encodedLength > allocatedBytes ||
        reference.blockIndex >
            std::numeric_limits<std::uint64_t>::max() / Limits::allocationQuantumBytes) {
        throwCorrupt("extent reference bounds are invalid");
    }
    const auto offset = reference.blockIndex * Limits::allocationQuantumBytes;
    if (offset > highWaterBytes || allocatedBytes > highWaterBytes - offset) {
        throwCorrupt("extent reference exceeds the committed boundary");
    }
}

[[nodiscard]] inline std::size_t commonPrefixLength(
    ByteView first,
    ByteView last) noexcept {
    std::size_t length = 0;
    while (length != first.size() && length != last.size() &&
           first[length] == last[length]) {
        ++length;
    }
    return length;
}

[[nodiscard]] inline std::array<std::byte, 16> allocatorRunKey(
    const ExtentRun& run,
    bool retired) {
    std::array<std::byte, 16> key{};
    if (retired) {
        writeLittleEndian<std::uint64_t>(run.retirementGeneration, key, 0);
        writeLittleEndian<std::uint64_t>(run.start, key, 8);
    } else {
        writeLittleEndian<std::uint64_t>(run.start, key, 0);
    }
    return key;
}

template<class Runs>
[[nodiscard]] inline std::uint64_t allocatorIndexUsedLength(
    const Runs& runs,
    std::size_t begin,
    std::size_t end,
    bool retired) {
    const auto keyLength = retired ? 16U : 8U;
    const auto first = allocatorRunKey(runs[begin], retired);
    std::size_t prefixLength = keyLength;
    for (auto index = begin + 1; index != end; ++index) {
        const auto key = allocatorRunKey(runs[index], retired);
        prefixLength = std::min(
            prefixLength,
            commonPrefixLength(
                ByteView{first}.first(prefixLength),
                ByteView{key}.first(prefixLength)));
    }
    const auto count = end - begin;
    return PageLayout::bytes + prefixLength + count * 8ULL +
        count * (12ULL + keyLength - prefixLength);
}

template<class Limits, class Allocator, class Runs>
[[nodiscard]] inline StoredBytes<Allocator> encodeAllocatorIndexLeaf(
    const Runs& runs,
    std::size_t begin,
    std::size_t end,
    bool retired,
    const Allocator& allocator) {
    using ByteAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<std::byte>;
    constexpr auto payloadBytes = std::max<std::uint64_t>(
        16U * 1024U, Limits::allocationQuantumBytes) -
        ExtentLayout::bytes - authenticationTagBytes;
    StoredBytes<Allocator> payload{ByteAllocator{allocator}};
    payload.resize(payloadBytes);
    StoredVector<std::array<std::byte, 16>, Allocator> keys{
        typename std::allocator_traits<Allocator>::
            template rebind_alloc<std::array<std::byte, 16>>{allocator}};
    keys.resize(end - begin);
    const auto keyLength = retired ? 16U : 8U;
    for (std::size_t index = 0; index != keys.size(); ++index) {
        keys[index] = allocatorRunKey(runs[begin + index], retired);
    }
    std::size_t prefixLength = keyLength;
    for (std::size_t index = 1; index != keys.size(); ++index) {
        prefixLength = std::min(
            prefixLength,
            commonPrefixLength(
                ByteView{keys.front()}.first(prefixLength),
                ByteView{keys[index]}.first(prefixLength)));
    }
    const auto slotsOffset = PageLayout::bytes + prefixLength;
    const auto entriesOffset = slotsOffset + keys.size() * 8U;
    const auto entryLength = 4U + keyLength - prefixLength + 8U;
    const auto usedLength = entriesOffset + keys.size() * entryLength;
    if (keys.empty() || usedLength > payload.size()) {
        throw DatabaseError{
            Errc::ResourceLimit,
            "allocator index requires more than one metadata leaf"};
    }
    MutableByteView output{payload};
    writeBytes(output, PageLayout::magic, "MIAREPG\0");
    writeLittleEndian<std::uint16_t>(1, output, PageLayout::version);
    writeLittleEndian<std::uint16_t>(1, output, PageLayout::type);
    writeLittleEndian<std::uint32_t>(PageLayout::bytes, output, PageLayout::headerLength);
    writeLittleEndian<std::uint32_t>(retired ? 5 : 4, output, PageLayout::role);
    writeLittleEndian<std::uint32_t>(keys.size(), output, PageLayout::entryCount);
    writeLittleEndian<std::uint32_t>(prefixLength, output, PageLayout::prefixLength);
    writeLittleEndian<std::uint32_t>(slotsOffset, output, PageLayout::slotsOffset);
    writeLittleEndian<std::uint32_t>(entriesOffset, output, PageLayout::entriesOffset);
    writeLittleEndian<std::uint32_t>(usedLength, output, PageLayout::usedLength);
    writeBytes(output, PageLayout::bytes, ByteView{keys.front()}.first(prefixLength));
    std::size_t entryOffset = entriesOffset;
    for (std::size_t index = 0; index != keys.size(); ++index) {
        const auto slot = slotsOffset + index * 8U;
        writeLittleEndian<std::uint32_t>(entryOffset, output, slot);
        writeLittleEndian<std::uint32_t>(entryLength, output, slot + 4);
        writeLittleEndian<std::uint32_t>(
            keyLength - prefixLength, output, entryOffset);
        writeBytes(
            output,
            entryOffset + 4,
            ByteView{keys[index]}.subspan(prefixLength, keyLength - prefixLength));
        writeLittleEndian<std::uint64_t>(
            runs[begin + index].count,
            output,
            entryOffset + 4 + keyLength - prefixLength);
        entryOffset += entryLength;
    }
    return payload;
}

[[nodiscard]] inline std::array<std::byte, 240> extentAssociatedData(
    const OpenedDatabase& opened,
    ByteView preamble) {
    std::array<std::byte, 240> data{};
    writeBytes(data, 0, "MiareExtentV1");
    writeBytes(
        data,
        16,
        ByteView{opened.bootstrap}.subspan(
            BootstrapLayout::databaseIdentity, databaseIdentityBytes));
    writeLittleEndian<std::uint32_t>(commonFormatVersion, data, 32);
    writeLittleEndian<std::uint32_t>(btreeBackendIdentifier, data, 36);
    writeLittleEndian<std::uint32_t>(1, data, 40);
    writeLittleEndian<std::uint32_t>(xchachaSuiteIdentifier, data, 44);
    writeBytes(
        data,
        48,
        ByteView{opened.publication}.subspan(
            PublicationLayout::capacityProfileDigest, cryptoKeyBytes));
    writeBytes(data, 80, preamble);
    return data;
}

template<class Allocator>
struct PreparedExactExtent {
    ExtentReference reference;
    StoredBytes<Allocator> bytes;
};

template<class Allocator>
struct PersistedLeafEntry {
    const StoredBytes<Allocator>* key;
    const StoredBytes<Allocator>* value;
    ExtentReference overflow;
};

template<class Allocator>
struct FixedLeafEntry {
    StoredBytes<Allocator> key;
    ExtentReference reference;
};

template<class Allocator>
struct PreparedTreeNode {
    ExtentReference reference;
    StoredBytes<Allocator> minimumKey;
    std::uint32_t level;
};

template<class Limits, class Allocator>
[[nodiscard]] inline PreparedExactExtent<Allocator> prepareAuthenticatedExtent(
    ByteView decoded,
    std::uint16_t unitKind,
    std::uint64_t generation,
    std::uint64_t blockIndex,
    OpenedDatabase& opened,
    ProviderSet& providers,
    const Allocator& allocator,
    bool compressionEligible = true,
    std::uint32_t keyDomain = 2,
    std::uint64_t sequence = 0,
    ByteView owner = {}) {
    using ByteAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<std::byte>;
    auto& crypto = ProviderAccess::crypto(providers);
    StoredBytes<Allocator> stored{ByteAllocator{allocator}};
    stored.assign(decoded.begin(), decoded.end());
    std::uint32_t flags = 0;
    std::uint32_t codec = 0;
    if (compressionEligible && opened.format.compression == Compression::ZStd &&
        !decoded.empty()) {
        auto& compression = ProviderAccess::compression(providers);
        StoredBytes<Allocator> candidate{ByteAllocator{allocator}};
        const auto candidateBytes = compression.compressBound(decoded.size());
        if (candidateBytes > ZSTD_compressBound(decoded.size())) {
            throw DatabaseError{
                Errc::ProviderUnavailable,
                "compression provider requested excessive output storage"};
        }
        candidate.resize(candidateBytes);
        const auto compressedBytes = compression.compress(decoded, candidate);
        if (compressedBytes > candidate.size()) {
            throw DatabaseError{
                Errc::ProviderUnavailable,
                "compression provider exceeded its output bound"};
        }
        candidate.resize(compressedBytes);
        const auto compressedBlocks =
            (ExtentLayout::bytes + compressedBytes + authenticationTagBytes +
             Limits::allocationQuantumBytes - 1) /
            Limits::allocationQuantumBytes;
        const auto uncompressedBlocks =
            (ExtentLayout::bytes + decoded.size() + authenticationTagBytes +
             Limits::allocationQuantumBytes - 1) /
            Limits::allocationQuantumBytes;
        if (compressedBytes < decoded.size() && compressedBlocks < uncompressedBlocks) {
            StoredBytes<Allocator> verified{ByteAllocator{allocator}};
            verified.resize(decoded.size());
            try {
                compression.decompress(candidate, verified);
            } catch (const ContractError&) {
                throw DatabaseError{
                    Errc::ProviderUnavailable,
                    "compression provider produced an invalid frame"};
            } catch (const DatabaseError& error) {
                if (error.code() != Errc::Corrupt) {
                    throw;
                }
                throw DatabaseError{
                    Errc::ProviderUnavailable,
                    "compression provider produced an invalid frame"};
            }
            if (!std::equal(decoded.begin(), decoded.end(), verified.begin())) {
                throw DatabaseError{
                    Errc::ProviderUnavailable,
                    "compression provider failed to preserve the payload"};
            }
            stored = std::move(candidate);
            flags = 1;
            codec = 1;
        }
    }

    const auto encodedLength =
        ExtentLayout::bytes + stored.size() + authenticationTagBytes;
    const auto blockCount =
        (encodedLength + Limits::allocationQuantumBytes - 1) /
        Limits::allocationQuantumBytes;
    ExtentReference reference{
        blockIndex,
        blockCount,
        encodedLength,
        generation};
    StoredBytes<Allocator> extent{ByteAllocator{allocator}};
    extent.resize(blockCount * Limits::allocationQuantumBytes);
    MutableByteView output{extent};
    writeBytes(output, ExtentLayout::magic, "MIAREXT\0");
    writeLittleEndian<std::uint16_t>(1, output, ExtentLayout::version);
    writeLittleEndian<std::uint16_t>(unitKind, output, ExtentLayout::unitKind);
    writeLittleEndian<std::uint32_t>(flags, output, ExtentLayout::flags);
    writeLittleEndian<std::uint32_t>(
        keyDomain, output, ExtentLayout::keyDomain);
    writeLittleEndian<std::uint32_t>(codec, output, ExtentLayout::codec);
    writeLittleEndian<std::uint32_t>(codec, output, ExtentLayout::codecProfile);
    writeLittleEndian<std::uint32_t>(
        ExtentLayout::bytes, output, ExtentLayout::preambleLength);
    writeLittleEndian<std::uint64_t>(blockIndex, output, ExtentLayout::blockIndex);
    writeLittleEndian<std::uint64_t>(blockCount, output, ExtentLayout::blockCount);
    writeLittleEndian<std::uint64_t>(encodedLength, output, ExtentLayout::encodedLength);
    writeLittleEndian<std::uint64_t>(stored.size(), output, ExtentLayout::storedLength);
    writeLittleEndian<std::uint64_t>(decoded.size(), output, ExtentLayout::decodedLength);
    writeLittleEndian<std::uint64_t>(generation, output, ExtentLayout::generation);
    writeLittleEndian<std::uint64_t>(sequence, output, ExtentLayout::sequence);
    if (!owner.empty()) {
        if (owner.size() != BlobId::encodedSize) {
            throw ContractError{
                Errc::InvalidArgument,
                "authenticated extent owner must be a Blob identifier"};
        }
        writeBytes(output, ExtentLayout::owner, owner);
    }
    std::array<std::byte, aeadNonceBytes> nonce{};
    crypto.randomBytes(nonce);
    writeBytes(output, ExtentLayout::nonce, nonce);
    const auto associatedData = extentAssociatedData(
        opened, ByteView{extent}.first(ExtentLayout::bytes));
    crypto.encryptDetached(
        keyDomain == 4 ? opened.keys.blob.view() : opened.keys.mainData.view(),
        nonce,
        stored,
        associatedData,
        MutableByteView{extent}.subspan(ExtentLayout::bytes, stored.size()),
        MutableByteView{extent}.subspan(
            ExtentLayout::bytes + stored.size(), authenticationTagBytes));
    return PreparedExactExtent<Allocator>{reference, std::move(extent)};
}

template<class Limits, class Allocator, class Entries>
[[nodiscard]] inline std::uint64_t leafUsedLength(
    const Entries& entries,
    std::size_t begin,
    std::size_t end) {
    const auto prefixLength = commonPrefixLength(
        *entries[begin].key, *entries[end - 1].key);
    std::uint64_t used = PageLayout::bytes + prefixLength + (end - begin) * 8ULL;
    for (auto index = begin; index != end; ++index) {
        const auto& entry = entries[index];
        used += 4ULL + entry.key->size() - prefixLength + 16ULL +
            (entry.overflow.null() ? entry.value->size() : 32ULL);
    }
    return used;
}

template<class Limits, class Allocator, class Entries>
[[nodiscard]] inline StoredBytes<Allocator> encodeLeafPage(
    const Entries& entries,
    std::size_t begin,
    std::size_t end,
    const Allocator& allocator) {
    using ByteAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<std::byte>;
    constexpr auto payloadBytes = std::max<std::uint64_t>(
        16U * 1024U, Limits::allocationQuantumBytes) -
        ExtentLayout::bytes - authenticationTagBytes;
    StoredBytes<Allocator> payload{ByteAllocator{allocator}};
    payload.resize(payloadBytes);
    const auto prefixLength = commonPrefixLength(
        *entries[begin].key, *entries[end - 1].key);
    const auto count = end - begin;
    const auto used = leafUsedLength<Limits, Allocator>(entries, begin, end);
    if (used > payload.size()) {
        throw DatabaseError{Errc::ResourceLimit, "one key/value cannot fit in a leaf page"};
    }
    MutableByteView output{payload};
    writeBytes(output, PageLayout::magic, "MIAREPG\0");
    writeLittleEndian<std::uint16_t>(1, output, PageLayout::version);
    writeLittleEndian<std::uint16_t>(1, output, PageLayout::type);
    writeLittleEndian<std::uint32_t>(PageLayout::bytes, output, PageLayout::headerLength);
    writeLittleEndian<std::uint32_t>(1, output, PageLayout::role);
    writeLittleEndian<std::uint32_t>(0, output, PageLayout::level);
    writeLittleEndian<std::uint32_t>(count, output, PageLayout::entryCount);
    writeLittleEndian<std::uint32_t>(prefixLength, output, PageLayout::prefixLength);
    const auto slotsOffset = PageLayout::bytes + prefixLength;
    const auto entriesOffset = slotsOffset + count * 8U;
    writeLittleEndian<std::uint32_t>(slotsOffset, output, PageLayout::slotsOffset);
    writeLittleEndian<std::uint32_t>(entriesOffset, output, PageLayout::entriesOffset);
    writeLittleEndian<std::uint32_t>(used, output, PageLayout::usedLength);
    writeBytes(output, PageLayout::bytes, ByteView{*entries[begin].key}.first(prefixLength));
    std::size_t slot = slotsOffset;
    std::size_t position = entriesOffset;
    for (auto index = begin; index != end; ++index) {
        const auto& item = entries[index];
        const auto suffixLength = item.key->size() - prefixLength;
        const auto payloadLength = item.overflow.null() ? item.value->size() : 32U;
        const auto entryLength = 4U + suffixLength + 16U + payloadLength;
        writeLittleEndian<std::uint32_t>(position, output, slot);
        writeLittleEndian<std::uint32_t>(entryLength, output, slot + 4);
        writeLittleEndian<std::uint32_t>(suffixLength, output, position);
        writeBytes(output, position + 4, ByteView{*item.key}.subspan(prefixLength));
        const auto representation = position + 4 + suffixLength;
        output[representation] = item.overflow.null() ? std::byte{0} : std::byte{1};
        writeLittleEndian<std::uint64_t>(item.value->size(), output, representation + 8);
        if (item.overflow.null()) {
            writeBytes(output, representation + 16, *item.value);
        } else {
            writeBytes(output, representation + 16, encodeExtentReference(item.overflow));
        }
        slot += 8;
        position += entryLength;
    }
    return payload;
}

template<class Entries>
[[nodiscard]] inline std::uint64_t fixedLeafUsedLength(
    const Entries& entries,
    std::size_t begin,
    std::size_t end) {
    const auto prefixLength = commonPrefixLength(
        entries[begin].key, entries[end - 1].key);
    std::uint64_t used =
        PageLayout::bytes + prefixLength + (end - begin) * 8ULL;
    for (auto index = begin; index != end; ++index) {
        used += 4ULL + entries[index].key.size() - prefixLength + 32ULL;
    }
    return used;
}

template<class Limits, class Allocator, class Entries>
[[nodiscard]] inline StoredBytes<Allocator> encodeFixedLeafPage(
    const Entries& entries,
    std::size_t begin,
    std::size_t end,
    std::uint32_t role,
    const Allocator& allocator) {
    using ByteAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<std::byte>;
    constexpr auto payloadBytes = std::max<std::uint64_t>(
        16U * 1024U, Limits::allocationQuantumBytes) -
        ExtentLayout::bytes - authenticationTagBytes;
    StoredBytes<Allocator> payload{ByteAllocator{allocator}};
    payload.resize(payloadBytes);
    const auto prefixLength = commonPrefixLength(
        entries[begin].key, entries[end - 1].key);
    const auto count = end - begin;
    const auto used = fixedLeafUsedLength(entries, begin, end);
    MutableByteView output{payload};
    writeBytes(output, PageLayout::magic, "MIAREPG\0");
    writeLittleEndian<std::uint16_t>(1, output, PageLayout::version);
    writeLittleEndian<std::uint16_t>(1, output, PageLayout::type);
    writeLittleEndian<std::uint32_t>(
        PageLayout::bytes, output, PageLayout::headerLength);
    writeLittleEndian<std::uint32_t>(role, output, PageLayout::role);
    writeLittleEndian<std::uint32_t>(0, output, PageLayout::level);
    writeLittleEndian<std::uint32_t>(count, output, PageLayout::entryCount);
    writeLittleEndian<std::uint32_t>(
        prefixLength, output, PageLayout::prefixLength);
    const auto slotsOffset = PageLayout::bytes + prefixLength;
    const auto entriesOffset = slotsOffset + count * 8U;
    writeLittleEndian<std::uint32_t>(
        slotsOffset, output, PageLayout::slotsOffset);
    writeLittleEndian<std::uint32_t>(
        entriesOffset, output, PageLayout::entriesOffset);
    writeLittleEndian<std::uint32_t>(used, output, PageLayout::usedLength);
    writeBytes(
        output,
        PageLayout::bytes,
        ByteView{entries[begin].key}.first(prefixLength));
    std::size_t slot = slotsOffset;
    std::size_t position = entriesOffset;
    for (auto index = begin; index != end; ++index) {
        const auto suffixLength = entries[index].key.size() - prefixLength;
        const auto entryLength = 4U + suffixLength + 32U;
        writeLittleEndian<std::uint32_t>(position, output, slot);
        writeLittleEndian<std::uint32_t>(entryLength, output, slot + 4);
        writeLittleEndian<std::uint32_t>(suffixLength, output, position);
        writeBytes(
            output,
            position + 4,
            ByteView{entries[index].key}.subspan(prefixLength));
        writeBytes(
            output,
            position + 4 + suffixLength,
            encodeExtentReference(entries[index].reference));
        slot += 8;
        position += entryLength;
    }
    return payload;
}

template<class Allocator, class Nodes>
[[nodiscard]] inline std::uint64_t internalUsedLength(
    const Nodes& nodes,
    std::size_t begin,
    std::size_t end) {
    std::size_t prefixLength = end - begin == 1
        ? 0
        : nodes[begin + 1].minimumKey.size();
    for (auto index = begin + 2; index < end; ++index) {
        prefixLength = std::min(
            prefixLength,
            commonPrefixLength(
                ByteView{nodes[begin + 1].minimumKey}.first(prefixLength),
                ByteView{nodes[index].minimumKey}.first(prefixLength)));
    }
    std::uint64_t used = PageLayout::bytes + prefixLength + (end - begin - 1) * 8ULL;
    for (auto index = begin + 1; index != end; ++index) {
        used += 4ULL + nodes[index].minimumKey.size() - prefixLength + 32ULL;
    }
    return used;
}

template<class Limits, class Allocator, class Nodes>
[[nodiscard]] inline StoredBytes<Allocator> encodeInternalPage(
    const Nodes& nodes,
    std::size_t begin,
    std::size_t end,
    const Allocator& allocator,
    std::uint32_t role = 1) {
    using ByteAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<std::byte>;
    constexpr auto payloadBytes = std::max<std::uint64_t>(
        16U * 1024U, Limits::allocationQuantumBytes) -
        ExtentLayout::bytes - authenticationTagBytes;
    StoredBytes<Allocator> payload{ByteAllocator{allocator}};
    payload.resize(payloadBytes);
    const auto count = end - begin - 1;
    std::size_t prefixLength = count == 0
        ? 0
        : nodes[begin + 1].minimumKey.size();
    for (auto index = begin + 2; index < end; ++index) {
        prefixLength = std::min(
            prefixLength,
            commonPrefixLength(
                ByteView{nodes[begin + 1].minimumKey}.first(prefixLength),
                ByteView{nodes[index].minimumKey}.first(prefixLength)));
    }
    const auto used = internalUsedLength<Allocator>(nodes, begin, end);
    if (used > payload.size()) {
        throw DatabaseError{Errc::ResourceLimit, "one separator cannot fit in an internal page"};
    }
    MutableByteView output{payload};
    writeBytes(output, PageLayout::magic, "MIAREPG\0");
    writeLittleEndian<std::uint16_t>(1, output, PageLayout::version);
    writeLittleEndian<std::uint16_t>(2, output, PageLayout::type);
    writeLittleEndian<std::uint32_t>(PageLayout::bytes, output, PageLayout::headerLength);
    writeLittleEndian<std::uint32_t>(role, output, PageLayout::role);
    writeLittleEndian<std::uint32_t>(nodes[begin].level + 1, output, PageLayout::level);
    writeLittleEndian<std::uint32_t>(count, output, PageLayout::entryCount);
    writeLittleEndian<std::uint32_t>(prefixLength, output, PageLayout::prefixLength);
    const auto slotsOffset = PageLayout::bytes + prefixLength;
    const auto entriesOffset = slotsOffset + count * 8U;
    writeLittleEndian<std::uint32_t>(slotsOffset, output, PageLayout::slotsOffset);
    writeLittleEndian<std::uint32_t>(entriesOffset, output, PageLayout::entriesOffset);
    writeLittleEndian<std::uint32_t>(used, output, PageLayout::usedLength);
    writeBytes(output, PageLayout::leftmostChild, encodeExtentReference(nodes[begin].reference));
    if (count != 0) {
        writeBytes(output, PageLayout::bytes, ByteView{nodes[begin + 1].minimumKey}.first(prefixLength));
    }
    std::size_t slot = slotsOffset;
    std::size_t position = entriesOffset;
    for (auto index = begin + 1; index != end; ++index) {
        const auto suffixLength = nodes[index].minimumKey.size() - prefixLength;
        const auto entryLength = 4U + suffixLength + 32U;
        writeLittleEndian<std::uint32_t>(position, output, slot);
        writeLittleEndian<std::uint32_t>(entryLength, output, slot + 4);
        writeLittleEndian<std::uint32_t>(suffixLength, output, position);
        writeBytes(output, position + 4, ByteView{nodes[index].minimumKey}.subspan(prefixLength));
        writeBytes(output, position + 4 + suffixLength, encodeExtentReference(nodes[index].reference));
        slot += 8;
        position += entryLength;
    }
    return payload;
}

template<class Limits, class Allocator>
[[nodiscard]] inline StoredBytes<Allocator> readAuthenticatedExtent(
    DurableFile& file,
    const ExtentReference& reference,
    std::uint16_t expectedKind,
    std::optional<std::uint64_t> expectedDecodedLength,
    OpenedDatabase& opened,
    ProviderSet& providers,
    const Allocator& allocator,
    bool compressionEligible = true,
    std::uint32_t keyDomain = 2,
    std::uint64_t sequence = 0,
    ByteView owner = {},
    std::optional<std::uint64_t> validationGeneration = std::nullopt,
    std::optional<std::uint64_t> validationHighWaterBytes = std::nullopt) {
    if (reference.null()) {
        throwCorrupt("authenticated extent reference is null");
    }
    validateExtentReference<Limits>(
        reference,
        validationGeneration.value_or(opened.format.generation),
        validationHighWaterBytes.value_or(opened.format.highWaterBytes));
    constexpr auto pageCeiling = std::max<std::uint64_t>(
        16U * 1024U, Limits::allocationQuantumBytes);
    constexpr auto uncompressedBlockCount =
        pageCeiling / Limits::allocationQuantumBytes;
    constexpr auto framingBytes =
        ExtentLayout::bytes + authenticationTagBytes;
    const auto minimalBlockCount =
        reference.encodedLength / Limits::allocationQuantumBytes +
        (reference.encodedLength % Limits::allocationQuantumBytes != 0);
    std::optional<std::uint64_t> expectedUncompressedBlockCount;
    if (expectedDecodedLength) {
        if (*expectedDecodedLength >
            std::numeric_limits<std::uint64_t>::max() - framingBytes -
                (Limits::allocationQuantumBytes - 1)) {
            throwCorrupt("authenticated extent decoded length overflows");
        }
        expectedUncompressedBlockCount =
            (framingBytes + *expectedDecodedLength +
             Limits::allocationQuantumBytes - 1) /
            Limits::allocationQuantumBytes;
    }
    const bool page = expectedKind >= 1 && expectedKind <= 10;
    if ((page && reference.blockCount > uncompressedBlockCount) ||
        (expectedUncompressedBlockCount &&
         reference.blockCount > *expectedUncompressedBlockCount) ||
        reference.blockCount != minimalBlockCount) {
        throwCorrupt("authenticated extent span is noncanonical");
    }
    using ByteAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<std::byte>;
    StoredBytes<Allocator> extent{ByteAllocator{allocator}};
    extent.resize(reference.blockCount * Limits::allocationQuantumBytes);
    const auto offset = reference.blockIndex * Limits::allocationQuantumBytes;
    file.readExactAt(offset, extent);
    const ByteView input{extent};
    if (!matches(input, ExtentLayout::magic, "MIAREXT\0") ||
        readLittleEndian<std::uint16_t>(input, ExtentLayout::version) != 1 ||
        readLittleEndian<std::uint16_t>(input, ExtentLayout::unitKind) != expectedKind ||
        readLittleEndian<std::uint32_t>(input, ExtentLayout::keyDomain) !=
            keyDomain ||
        readLittleEndian<std::uint32_t>(input, ExtentLayout::preambleLength) !=
            ExtentLayout::bytes ||
        readLittleEndian<std::uint64_t>(input, ExtentLayout::blockIndex) !=
            reference.blockIndex ||
        readLittleEndian<std::uint64_t>(input, ExtentLayout::blockCount) !=
            reference.blockCount ||
        readLittleEndian<std::uint64_t>(input, ExtentLayout::encodedLength) !=
            reference.encodedLength ||
        readLittleEndian<std::uint64_t>(input, ExtentLayout::generation) !=
            reference.creationGeneration ||
        readLittleEndian<std::uint64_t>(input, ExtentLayout::sequence) !=
            sequence ||
        (owner.empty()
             ? !allZero(input, ExtentLayout::owner, ExtentLayout::nonce)
             : (owner.size() != BlobId::encodedSize ||
                !std::equal(
                    owner.begin(),
                    owner.end(),
                    input.begin() + ExtentLayout::owner))) ||
        !allZero(input, ExtentLayout::reserved, ExtentLayout::bytes) ||
        !allZero(input, reference.encodedLength, extent.size())) {
        throwCorrupt("authenticated extent is noncanonical");
    }
    const auto flags = readLittleEndian<std::uint32_t>(input, ExtentLayout::flags);
    const auto codec = readLittleEndian<std::uint32_t>(input, ExtentLayout::codec);
    const auto codecProfile =
        readLittleEndian<std::uint32_t>(input, ExtentLayout::codecProfile);
    const auto storedLength =
        readLittleEndian<std::uint64_t>(input, ExtentLayout::storedLength);
    const auto decodedLength =
        readLittleEndian<std::uint64_t>(input, ExtentLayout::decodedLength);
    const auto expectedStoredLength = reference.encodedLength -
        ExtentLayout::bytes - authenticationTagBytes;
    if (decodedLength > std::numeric_limits<std::uint64_t>::max() -
            framingBytes - (Limits::allocationQuantumBytes - 1)) {
        throwCorrupt("authenticated extent decoded length overflows");
    }
    const auto uncompressedBlocks =
        (framingBytes + decodedLength +
         Limits::allocationQuantumBytes - 1) /
        Limits::allocationQuantumBytes;
    if (flags > 1 || (!compressionEligible && flags != 0) ||
        codec != flags || codecProfile != flags ||
        storedLength != expectedStoredLength ||
        (expectedDecodedLength && decodedLength != *expectedDecodedLength) ||
        (page && decodedLength != pageCeiling - ExtentLayout::bytes - authenticationTagBytes) ||
        (flags == 0 && storedLength != decodedLength) ||
        (flags == 1 &&
         (opened.format.compression != Compression::ZStd ||
          storedLength >= decodedLength ||
          reference.blockCount >= uncompressedBlocks))) {
        throwCorrupt("authenticated extent representation is invalid");
    }
    StoredBytes<Allocator> stored{ByteAllocator{allocator}};
    stored.resize(storedLength);
    const auto associatedData = extentAssociatedData(
        opened, input.first(ExtentLayout::bytes));
    auto& crypto = ProviderAccess::crypto(providers);
    if (!crypto.decryptDetached(
        keyDomain == 4 ? opened.keys.blob.view() : opened.keys.mainData.view(),
            input.subspan(ExtentLayout::nonce, aeadNonceBytes),
            input.subspan(ExtentLayout::bytes, storedLength),
            input.subspan(ExtentLayout::bytes + storedLength, authenticationTagBytes),
            associatedData,
            stored)) {
        throwCorrupt("authenticated extent authentication failed");
    }
    StoredBytes<Allocator> decoded{ByteAllocator{allocator}};
    decoded.resize(decodedLength);
    if (flags == 1) {
        ProviderAccess::compression(providers).decompress(stored, decoded);
    } else {
        decoded = std::move(stored);
    }
    return decoded;
}

template<class Allocator>
[[nodiscard]] inline ExtentRuns<Allocator> decodeAllocatorIndexLeaf(
    ByteView payload,
    bool retired,
    const Allocator& allocator) {
    const auto expectedRole = retired ? 5U : 4U;
    if (!matches(payload, PageLayout::magic, "MIAREPG\0") ||
        readLittleEndian<std::uint16_t>(payload, PageLayout::version) != 1 ||
        readLittleEndian<std::uint16_t>(payload, PageLayout::type) != 1 ||
        readLittleEndian<std::uint32_t>(payload, PageLayout::headerLength) !=
            PageLayout::bytes ||
        readLittleEndian<std::uint32_t>(payload, PageLayout::role) != expectedRole ||
        readLittleEndian<std::uint32_t>(payload, PageLayout::level) != 0 ||
        readLittleEndian<std::uint32_t>(payload, PageLayout::flags) != 0 ||
        !allZero(payload, PageLayout::leftmostChild, PageLayout::bytes)) {
        throwCorrupt("allocator index header is invalid");
    }
    const auto count = readLittleEndian<std::uint32_t>(payload, PageLayout::entryCount);
    const auto prefixLength = readLittleEndian<std::uint32_t>(payload, PageLayout::prefixLength);
    const auto keyLength = retired ? 16U : 8U;
    const auto slotsOffset = readLittleEndian<std::uint32_t>(payload, PageLayout::slotsOffset);
    const auto entriesOffset = readLittleEndian<std::uint32_t>(payload, PageLayout::entriesOffset);
    const auto usedLength = readLittleEndian<std::uint32_t>(payload, PageLayout::usedLength);
    if (count == 0 || prefixLength > keyLength ||
        slotsOffset != PageLayout::bytes + prefixLength ||
        entriesOffset != slotsOffset + static_cast<std::uint64_t>(count) * 8 ||
        usedLength < entriesOffset || usedLength > payload.size() ||
        !allZero(payload, usedLength, payload.size())) {
        throwCorrupt("allocator index bounds are invalid");
    }
    const auto prefix = payload.subspan(PageLayout::bytes, prefixLength);
    ExtentRuns<Allocator> runs{
        typename std::allocator_traits<Allocator>::
            template rebind_alloc<ExtentRun>{allocator}};
    runs.reserve(count);
    std::array<std::byte, 16> firstKey{};
    std::size_t canonicalPrefix = keyLength;
    std::size_t expectedEntry = entriesOffset;
    for (std::uint32_t index = 0; index != count; ++index) {
        const auto slot = slotsOffset + index * 8U;
        const auto entryOffset = readLittleEndian<std::uint32_t>(payload, slot);
        const auto entryLength = readLittleEndian<std::uint32_t>(payload, slot + 4);
        if (entryOffset > usedLength || usedLength - entryOffset < 4U) {
            throwCorrupt("allocator index entry is out of bounds");
        }
        const auto suffixLength = readLittleEndian<std::uint32_t>(payload, entryOffset);
        if (entryOffset != expectedEntry || suffixLength != keyLength - prefixLength ||
            entryLength != 12U + suffixLength ||
            entryOffset > usedLength || entryLength > usedLength - entryOffset) {
            throwCorrupt("allocator index entry is invalid");
        }
        std::array<std::byte, 16> key{};
        std::copy(prefix.begin(), prefix.end(), key.begin());
        std::copy_n(
            payload.begin() + entryOffset + 4,
            suffixLength,
            key.begin() + prefixLength);
        const auto generation = retired
            ? readLittleEndian<std::uint64_t>(key, 0)
            : 0;
        const auto start = readLittleEndian<std::uint64_t>(key, retired ? 8 : 0);
        const auto blockCount = readLittleEndian<std::uint64_t>(
            payload, entryOffset + 4 + suffixLength);
        if (blockCount == 0 ||
            (index != 0 &&
             (generation < runs.back().retirementGeneration ||
              (generation == runs.back().retirementGeneration &&
               start <= runs.back().start + runs.back().count)))) {
            throwCorrupt("allocator index runs are not canonical");
        }
        if (index == 0) {
            firstKey = key;
        } else {
            canonicalPrefix = std::min(
                canonicalPrefix,
                commonPrefixLength(
                    ByteView{firstKey}.first(canonicalPrefix),
                    ByteView{key}.first(canonicalPrefix)));
        }
        runs.push_back(ExtentRun{start, blockCount, generation});
        expectedEntry += entryLength;
    }
    if (expectedEntry != usedLength ||
        canonicalPrefix != prefixLength) {
        throwCorrupt("allocator index image is noncanonical");
    }
    return runs;
}

template<class Limits, class Allocator>
[[nodiscard]] inline ExtentRuns<Allocator> loadAllocatorIndexPage(
    DurableFile& file,
    const ExtentReference& reference,
    bool retired,
    std::optional<std::uint32_t> expectedLevel,
    OpenedDatabase& opened,
    ProviderSet& providers,
    const Allocator& allocator,
    ExtentReferences<Allocator>& reachable) {
    using RunAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<ExtentRun>;
    using ReferenceAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<ExtentReference>;
    using Key = std::array<std::byte, 16>;
    using KeyAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<Key>;
    std::array<std::byte, ExtentLayout::bytes> preamble{};
    validateExtentReference<Limits>(
        reference, opened.format.generation, opened.format.highWaterBytes);
    file.readExactAt(
        reference.blockIndex * Limits::allocationQuantumBytes, preamble);
    const auto kind = readLittleEndian<std::uint16_t>(
        preamble, ExtentLayout::unitKind);
    const auto internalKind = retired ? 9U : 7U;
    const auto leafKind = retired ? 10U : 8U;
    if (kind != internalKind && kind != leafKind) {
        throwCorrupt("allocator index page role is invalid");
    }
    auto payload = readAuthenticatedExtent<Limits>(
        file, reference, kind, std::nullopt,
        opened, providers, allocator);
    reachable.push_back(reference);
    const auto level = readLittleEndian<std::uint32_t>(
        payload, PageLayout::level);
    if ((expectedLevel && level != *expectedLevel) ||
        level > maximumTreeLevel ||
        (kind == leafKind) != (level == 0)) {
        throwCorrupt("allocator index level is invalid");
    }
    if (kind == leafKind) {
        return decodeAllocatorIndexLeaf<Allocator>(
            payload, retired, allocator);
    }

    const auto role = retired ? 5U : 4U;
    const auto count = readLittleEndian<std::uint32_t>(
        payload, PageLayout::entryCount);
    const auto prefixLength = readLittleEndian<std::uint32_t>(
        payload, PageLayout::prefixLength);
    const auto keyLength = retired ? 16U : 8U;
    const auto slotsOffset = readLittleEndian<std::uint32_t>(
        payload, PageLayout::slotsOffset);
    const auto entriesOffset = readLittleEndian<std::uint32_t>(
        payload, PageLayout::entriesOffset);
    const auto usedLength = readLittleEndian<std::uint32_t>(
        payload, PageLayout::usedLength);
    const ByteView input{payload};
    const auto leftmost = decodeExtentReference(
        input.subspan(PageLayout::leftmostChild, 32));
    if (!matches(payload, PageLayout::magic, "MIAREPG\0") ||
        readLittleEndian<std::uint16_t>(payload, PageLayout::version) != 1 ||
        readLittleEndian<std::uint16_t>(payload, PageLayout::type) != 2 ||
        readLittleEndian<std::uint32_t>(payload, PageLayout::headerLength) !=
            PageLayout::bytes ||
        readLittleEndian<std::uint32_t>(payload, PageLayout::role) != role ||
        readLittleEndian<std::uint32_t>(payload, PageLayout::flags) != 0 ||
        leftmost.null() || prefixLength > keyLength ||
        slotsOffset != PageLayout::bytes + prefixLength ||
        entriesOffset != slotsOffset + static_cast<std::uint64_t>(count) * 8 ||
        usedLength < entriesOffset || usedLength > payload.size() ||
        !allZero(payload, PageLayout::reserved, PageLayout::bytes) ||
        !allZero(payload, usedLength, payload.size())) {
        throwCorrupt("allocator internal page header is invalid");
    }
    StoredVector<Key, Allocator> separators{KeyAllocator{allocator}};
    ExtentReferences<Allocator> children{ReferenceAllocator{allocator}};
    separators.reserve(count);
    children.reserve(static_cast<std::size_t>(count) + 1);
    children.push_back(leftmost);
    const auto prefix = input.subspan(PageLayout::bytes, prefixLength);
    Key firstKey{};
    std::size_t canonicalPrefix = count == 0 ? 0 : keyLength;
    std::size_t expectedEntry = entriesOffset;
    for (std::uint32_t index = 0; index != count; ++index) {
        const auto slot = slotsOffset + index * 8U;
        const auto entryOffset = readLittleEndian<std::uint32_t>(payload, slot);
        const auto entryLength = readLittleEndian<std::uint32_t>(
            payload, slot + 4);
        if (entryOffset > usedLength || entryOffset + 4U > usedLength) {
            throwCorrupt("allocator internal entry is out of bounds");
        }
        const auto suffixLength = readLittleEndian<std::uint32_t>(
            payload, entryOffset);
        if (entryOffset != expectedEntry ||
            suffixLength != keyLength - prefixLength ||
            entryLength != 36U + suffixLength ||
            entryLength > usedLength - entryOffset) {
            throwCorrupt("allocator internal entry is invalid");
        }
        Key key{};
        std::copy(prefix.begin(), prefix.end(), key.begin());
        std::copy_n(
            payload.begin() + entryOffset + 4,
            suffixLength,
            key.begin() + prefixLength);
        const auto child = decodeExtentReference(input.subspan(
            entryOffset + 4 + suffixLength, 32));
        if (child.null()) {
            throwCorrupt("allocator internal child is null");
        }
        if (index == 0) {
            firstKey = key;
        } else {
            canonicalPrefix = std::min(
                canonicalPrefix,
                commonPrefixLength(
                    ByteView{firstKey}.first(canonicalPrefix),
                    ByteView{key}.first(canonicalPrefix)));
        }
        separators.push_back(key);
        children.push_back(child);
        expectedEntry += entryLength;
    }
    if (expectedEntry != usedLength || canonicalPrefix != prefixLength) {
        throwCorrupt("allocator internal page is noncanonical");
    }

    ExtentRuns<Allocator> result{RunAllocator{allocator}};
    for (std::size_t index = 0; index != children.size(); ++index) {
        auto childRuns = loadAllocatorIndexPage<Limits>(
            file,
            children[index],
            retired,
            level - 1,
            opened,
            providers,
            allocator,
            reachable);
        if (childRuns.empty() ||
            (index != 0 &&
             allocatorRunKey(childRuns.front(), retired) != separators[index - 1])) {
            throwCorrupt("allocator separator does not match its right subtree");
        }
        if (!result.empty()) {
            const auto& previous = result.back();
            const auto& next = childRuns.front();
            if (next.retirementGeneration < previous.retirementGeneration ||
                (next.retirementGeneration == previous.retirementGeneration &&
                 next.start <= previous.start + previous.count)) {
                throwCorrupt("allocator subtrees overlap or are reordered");
            }
        }
        result.insert(
            result.end(),
            std::make_move_iterator(childRuns.begin()),
            std::make_move_iterator(childRuns.end()));
    }
    return result;
}

template<class Allocator>
inline void validateAllocatorPartition(
    std::uint64_t commonBlocks,
    std::uint64_t highWaterBlocks,
    const ExtentReferences<Allocator>& reachable,
    const ExtentRuns<Allocator>& freeRuns,
    const ExtentRuns<Allocator>& retiredRuns,
    const Allocator& allocator) {
    using RunAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<ExtentRun>;
    ExtentRuns<Allocator> intervals{RunAllocator{allocator}};
    const auto add = [&](std::uint64_t start, std::uint64_t count) {
        if (start < commonBlocks || count == 0 || start > highWaterBlocks ||
            count > highWaterBlocks - start) {
            throwCorrupt("allocator partition range is out of bounds");
        }
        intervals.push_back(ExtentRun{start, count});
    };
    for (const auto& reference : reachable) {
        add(reference.blockIndex, reference.blockCount);
    }
    for (const auto& run : freeRuns) {
        add(run.start, run.count);
    }
    for (const auto& run : retiredRuns) {
        add(run.start, run.count);
    }
    std::sort(
        intervals.begin(), intervals.end(),
        [](const auto& left, const auto& right) {
            return left.start < right.start;
        });
    auto cursor = commonBlocks;
    for (const auto& interval : intervals) {
        if (interval.start < cursor) {
            throwCorrupt("allocator partition ranges overlap");
        }
        if (interval.start != cursor) {
            throwCorrupt("allocator partition leaves unclassified blocks");
        }
        cursor += interval.count;
    }
    if (cursor != highWaterBlocks) {
        throwCorrupt("allocator partition leaves unclassified blocks");
    }
}

template<class Allocator>
inline void subtractRetainedReferences(
    ExtentRuns<Allocator>& retiredRuns,
    const ExtentReferences<Allocator>& retainedReferences,
    const Allocator& allocator) {
    using RunAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<ExtentRun>;
    ExtentRuns<Allocator> retainedRuns{RunAllocator{allocator}};
    retainedRuns.reserve(retainedReferences.size());
    for (const auto& reference : retainedReferences) {
        retainedRuns.push_back(ExtentRun{
            reference.blockIndex,
            reference.blockCount});
    }
    std::sort(
        retainedRuns.begin(), retainedRuns.end(),
        [](const auto& left, const auto& right) {
            return left.start < right.start;
        });
    ExtentRuns<Allocator> coalescedRetainedRuns{RunAllocator{allocator}};
    for (const auto& run : retainedRuns) {
        if (!coalescedRetainedRuns.empty() &&
            run.start <= coalescedRetainedRuns.back().start +
                coalescedRetainedRuns.back().count) {
            const auto end = std::max(
                coalescedRetainedRuns.back().start +
                    coalescedRetainedRuns.back().count,
                run.start + run.count);
            coalescedRetainedRuns.back().count =
                end - coalescedRetainedRuns.back().start;
        } else {
            coalescedRetainedRuns.push_back(run);
        }
    }

    std::sort(
        retiredRuns.begin(), retiredRuns.end(),
        [](const auto& left, const auto& right) {
            return left.start < right.start;
        });
    ExtentRuns<Allocator> remaining{RunAllocator{allocator}};
    std::size_t retainedIndex = 0;
    for (const auto& run : retiredRuns) {
        const auto runEnd = run.start + run.count;
        while (retainedIndex != coalescedRetainedRuns.size() &&
               coalescedRetainedRuns[retainedIndex].start +
                       coalescedRetainedRuns[retainedIndex].count <=
                   run.start) {
            ++retainedIndex;
        }
        auto cursor = run.start;
        auto scan = retainedIndex;
        while (scan != coalescedRetainedRuns.size() &&
               coalescedRetainedRuns[scan].start < runEnd) {
            const auto retainedStart = coalescedRetainedRuns[scan].start;
            const auto retainedEnd = retainedStart +
                coalescedRetainedRuns[scan].count;
            if (retainedStart > cursor) {
                remaining.push_back(ExtentRun{
                    cursor,
                    std::min(retainedStart, runEnd) - cursor,
                    run.retirementGeneration});
            }
            cursor = std::max(cursor, std::min(retainedEnd, runEnd));
            if (retainedEnd > runEnd) {
                break;
            }
            ++scan;
        }
        if (cursor < runEnd) {
            remaining.push_back(ExtentRun{
                cursor,
                runEnd - cursor,
                run.retirementGeneration});
        }
        retainedIndex = scan;
    }
    std::sort(
        remaining.begin(), remaining.end(),
        [](const auto& left, const auto& right) {
            if (left.retirementGeneration != right.retirementGeneration) {
                return left.retirementGeneration < right.retirementGeneration;
            }
            return left.start < right.start;
        });
    retiredRuns = std::move(remaining);
}

template<class Limits, class Allocator>
[[nodiscard]] inline std::optional<StoredBytes<Allocator>>
shallowValidateAllocatorRoot(
    DurableFile& file,
    OpenedDatabase& opened,
    ProviderSet& providers,
    const Allocator& allocator) {
    const auto root = decodeExtentReference(opened.format.allocatorRoot);
    if (opened.format.generation == 1) {
        if (!root.null()) {
            throwCorrupt("initial database has allocator state");
        }
        return std::nullopt;
    }
    if (root.null()) {
        throwCorrupt("committed generation has no allocator root");
    }
    if (root.creationGeneration != opened.format.generation) {
        throwCorrupt("allocator root generation does not match publication");
    }
    auto payload = readAuthenticatedExtent<Limits>(
        file,
        root,
        14,
        AllocatorRootLayout::bytes,
        opened,
        providers,
        allocator,
        false);
    const auto freeRoot = decodeExtentReference(
        ByteView{payload}.subspan(AllocatorRootLayout::freeRoot, 32));
    const auto retiredRoot = decodeExtentReference(
        ByteView{payload}.subspan(AllocatorRootLayout::retiredRoot, 32));
    const auto freeBlocks = readLittleEndian<std::uint64_t>(
        payload, AllocatorRootLayout::freeBlocks);
    const auto retiredBlocks = readLittleEndian<std::uint64_t>(
        payload, AllocatorRootLayout::retiredBlocks);
    if (!matches(payload, AllocatorRootLayout::magic, "MIAREALC") ||
        readLittleEndian<std::uint16_t>(
            payload, AllocatorRootLayout::version) != 1 ||
        readLittleEndian<std::uint16_t>(
            payload, AllocatorRootLayout::flags) != 0 ||
        readLittleEndian<std::uint32_t>(
            payload, AllocatorRootLayout::length) !=
            AllocatorRootLayout::bytes ||
        readLittleEndian<std::uint64_t>(
            payload, AllocatorRootLayout::generation) !=
            opened.format.generation ||
        readLittleEndian<std::uint64_t>(
            payload, AllocatorRootLayout::highWaterBlocks) !=
            opened.format.highWaterBytes / Limits::allocationQuantumBytes ||
        freeRoot.null() != (freeBlocks == 0) ||
        retiredRoot.null() != (retiredBlocks == 0) ||
        !allZero(
            payload,
            AllocatorRootLayout::reserved,
            AllocatorRootLayout::bytes)) {
        throwCorrupt("allocator root is noncanonical");
    }
    validateExtentReference<Limits>(
        freeRoot,
        opened.format.generation,
        opened.format.highWaterBytes);
    validateExtentReference<Limits>(
        retiredRoot,
        opened.format.generation,
        opened.format.highWaterBytes);
    return payload;
}

template<class Limits, class Allocator>
inline void loadAllocatorReferences(
    DurableFile& file,
    OpenedDatabase& opened,
    ProviderSet& providers,
    const Allocator& allocator,
    ExtentReferences<Allocator>& reachable,
    ExtentRuns<Allocator>* loadedFreeRuns = nullptr,
    ExtentRuns<Allocator>* loadedRetiredRuns = nullptr) {
    const auto root = decodeExtentReference(opened.format.allocatorRoot);
    auto payloadResult = shallowValidateAllocatorRoot<Limits>(
        file, opened, providers, allocator);
    if (!payloadResult) {
        return;
    }
    auto payload = std::move(*payloadResult);
    reachable.push_back(root);
    const auto addIndex = [&](std::size_t offset, bool retired) {
        const auto reference = decodeExtentReference(ByteView{payload}.subspan(offset, 32));
        if (reference.null()) {
            return ExtentRuns<Allocator>{
                typename std::allocator_traits<Allocator>::
                    template rebind_alloc<ExtentRun>{allocator}};
        }
        return loadAllocatorIndexPage<Limits>(
            file,
            reference,
            retired,
            std::nullopt,
            opened,
            providers,
            allocator,
            reachable);
    };
    auto freeRuns = addIndex(AllocatorRootLayout::freeRoot, false);
    auto retiredRuns = addIndex(AllocatorRootLayout::retiredRoot, true);
    std::uint64_t freeBlocks = 0;
    for (const auto& run : freeRuns) {
        freeBlocks += run.count;
    }
    std::uint64_t retiredBlocks = 0;
    for (const auto& run : retiredRuns) {
        if (run.retirementGeneration == 0 ||
            run.retirementGeneration > opened.format.generation) {
            throwCorrupt("allocator retirement generation is invalid");
        }
        retiredBlocks += run.count;
    }
    std::uint64_t reachableBlocks = 0;
    for (const auto& reference : reachable) {
        reachableBlocks += reference.blockCount;
    }
    if (freeBlocks != readLittleEndian<std::uint64_t>(
            payload, AllocatorRootLayout::freeBlocks) ||
        retiredBlocks != readLittleEndian<std::uint64_t>(
            payload, AllocatorRootLayout::retiredBlocks) ||
        reachableBlocks != readLittleEndian<std::uint64_t>(
            payload, AllocatorRootLayout::reachableBlocks) ||
        reachableBlocks + freeBlocks + retiredBlocks !=
            opened.format.highWaterBytes / Limits::allocationQuantumBytes -
                commonRegionBytes / Limits::allocationQuantumBytes) {
        throwCorrupt("allocator counters do not partition committed storage");
    }
    const auto commonBlocks = commonRegionBytes / Limits::allocationQuantumBytes;
    const auto highWaterBlocks =
        opened.format.highWaterBytes / Limits::allocationQuantumBytes;
    validateAllocatorPartition(
        commonBlocks,
        highWaterBlocks,
        reachable,
        freeRuns,
        retiredRuns,
        allocator);
    if (loadedFreeRuns) {
        *loadedFreeRuns = std::move(freeRuns);
    }
    if (loadedRetiredRuns) {
        *loadedRetiredRuns = std::move(retiredRuns);
    }
}

template<class Limits, class Allocator>
[[nodiscard]] inline AllocatorSnapshot<Allocator> loadAllocatorSnapshot(
    DurableFile& file,
    OpenedDatabase& opened,
    ProviderSet& providers,
    const Allocator& allocator) {
    using Snapshot = AllocatorSnapshot<Allocator>;
    using RetirementAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<std::pair<const std::uint64_t, std::uint64_t>>;
    const auto commonBlocks = commonRegionBytes / Limits::allocationQuantumBytes;
    Snapshot snapshot{
        commonBlocks,
        typename Snapshot::RetirementCounts{
            std::less<std::uint64_t>{}, RetirementAllocator{allocator}}};
    auto payloadResult = shallowValidateAllocatorRoot<Limits>(
        file, opened, providers, allocator);
    if (!payloadResult) {
        return snapshot;
    }

    const auto& payload = *payloadResult;
    const auto reachableBlocks = readLittleEndian<std::uint64_t>(
        payload, AllocatorRootLayout::reachableBlocks);
    snapshot.liveBlocks += reachableBlocks;
    const auto retiredRoot = decodeExtentReference(
        ByteView{payload}.subspan(AllocatorRootLayout::retiredRoot, 32));
    if (retiredRoot.null()) {
        return snapshot;
    }

    ExtentReferences<Allocator> indexReferences{
        typename std::allocator_traits<Allocator>::
            template rebind_alloc<ExtentReference>{allocator}};
    const auto retiredRuns = loadAllocatorIndexPage<Limits>(
        file,
        retiredRoot,
        true,
        std::nullopt,
        opened,
        providers,
        allocator,
        indexReferences);
    std::uint64_t retiredBlocks = 0;
    for (const auto& run : retiredRuns) {
        if (run.retirementGeneration == 0 ||
            run.retirementGeneration > opened.format.generation ||
            run.count > std::numeric_limits<std::uint64_t>::max() - retiredBlocks) {
            throwCorrupt("allocator retirement diagnostics are invalid");
        }
        retiredBlocks += run.count;
        snapshot.retiredBlocksByGeneration[run.retirementGeneration] += run.count;
    }
    if (retiredBlocks != readLittleEndian<std::uint64_t>(
            payload, AllocatorRootLayout::retiredBlocks)) {
        throwCorrupt("allocator retirement diagnostics contradict their counter");
    }
    return snapshot;
}

template<class Limits>
inline void validateOrderedPageReference(
    const ExtentReference& reference,
    std::uint64_t generation,
    std::uint64_t highWaterBytes) {
    validateExtentReference<Limits>(reference, generation, highWaterBytes);
    constexpr auto pageBlocks = std::max<std::uint64_t>(
        16U * 1024U, Limits::allocationQuantumBytes) /
        Limits::allocationQuantumBytes;
    const auto minimalBlocks =
        reference.encodedLength / Limits::allocationQuantumBytes +
        (reference.encodedLength % Limits::allocationQuantumBytes != 0);
    if (reference.blockCount > pageBlocks ||
        reference.blockCount != minimalBlocks) {
        throwCorrupt("ordered page extent span is noncanonical");
    }
}

template<class Allocator>
struct OrderedPageBounds {
    StoredBytes<Allocator> minimum;
    StoredBytes<Allocator> maximum;
};

template<class Allocator>
struct MutableTreeValue {
    StoredBytes<Allocator> key;
    StoredBytes<Allocator> value;
    ExtentReference overflow;
};

template<class Allocator>
struct MutableTreeNode {
    explicit MutableTreeNode(const Allocator& allocator)
        : minimumKey(
              typename std::allocator_traits<Allocator>::
                  template rebind_alloc<std::byte>{allocator}),
          values(
              typename std::allocator_traits<Allocator>::
                  template rebind_alloc<MutableTreeValue<Allocator>>{allocator}),
          children(
              typename std::allocator_traits<Allocator>::
                  template rebind_alloc<MutableTreeNode<Allocator>>{allocator}) {}

    ExtentReference reference;
    StoredBytes<Allocator> minimumKey;
    std::uint32_t level = 0;
    bool dirty = false;
    StoredVector<MutableTreeValue<Allocator>, Allocator> values;
    StoredVector<MutableTreeNode<Allocator>, Allocator> children;
};

template<class Allocator>
[[nodiscard]] inline MutableTreeNode<Allocator> cloneMutableTree(
    const MutableTreeNode<Allocator>& source,
    const Allocator& allocator) {
    using ByteAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<std::byte>;
    MutableTreeNode<Allocator> clone{allocator};
    clone.reference = source.reference;
    clone.minimumKey.assign(
        source.minimumKey.begin(), source.minimumKey.end());
    clone.level = source.level;
    clone.dirty = source.dirty;
    clone.values.reserve(source.values.size());
    for (const auto& sourceValue : source.values) {
        StoredBytes<Allocator> key{ByteAllocator{allocator}};
        StoredBytes<Allocator> value{ByteAllocator{allocator}};
        key.assign(sourceValue.key.begin(), sourceValue.key.end());
        value.assign(sourceValue.value.begin(), sourceValue.value.end());
        clone.values.push_back(MutableTreeValue<Allocator>{
            std::move(key),
            std::move(value),
            sourceValue.overflow});
    }
    clone.children.reserve(source.children.size());
    for (const auto& sourceChild : source.children) {
        clone.children.push_back(cloneMutableTree(sourceChild, allocator));
    }
    return clone;
}

template<class Limits, class Allocator>
inline OrderedPageBounds<Allocator> loadOrderedPage(
    DurableFile& file,
    const ExtentReference& reference,
    std::uint32_t expectedLevel,
    OpenedDatabase& opened,
    ProviderSet& providers,
    const Allocator& allocator,
    OrderedKeyValues<Allocator>& values,
    ExtentReferences<Allocator>* reachable = nullptr,
    MutableTreeNode<Allocator>* mutableNode = nullptr) {
    if (expectedLevel > maximumTreeLevel) {
        throwCorrupt("ordered tree level exceeds the supported depth");
    }
    using ByteAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<std::byte>;
    validateOrderedPageReference<Limits>(
        reference, opened.format.generation, opened.format.highWaterBytes);
    if (reachable) {
        reachable->push_back(reference);
    }
    std::array<std::byte, ExtentLayout::bytes> preamble{};
    file.readExactAt(reference.blockIndex * Limits::allocationQuantumBytes, preamble);
    const auto kind = readLittleEndian<std::uint16_t>(preamble, ExtentLayout::unitKind);
    if (kind != 1 && kind != 2) {
        throwCorrupt("ordered page role is invalid");
    }
    auto payload = readAuthenticatedExtent<Limits>(
        file, reference, kind, std::nullopt, opened, providers, allocator);
    const auto type = readLittleEndian<std::uint16_t>(payload, PageLayout::type);
    const auto level = readLittleEndian<std::uint32_t>(payload, PageLayout::level);
    if (mutableNode) {
        mutableNode->reference = reference;
        mutableNode->level = level;
    }
    if (!matches(payload, PageLayout::magic, "MIAREPG\0") ||
        readLittleEndian<std::uint16_t>(payload, PageLayout::version) != 1 ||
        (type != 1 && type != 2) || kind != (type == 1 ? 2 : 1) ||
        readLittleEndian<std::uint32_t>(payload, PageLayout::headerLength) != PageLayout::bytes ||
        readLittleEndian<std::uint32_t>(payload, PageLayout::role) != 1 ||
        level != expectedLevel || (type == 1) != (level == 0) ||
        readLittleEndian<std::uint32_t>(payload, PageLayout::flags) != 0 ||
        !allZero(payload, PageLayout::reserved, PageLayout::bytes)) {
        throwCorrupt("ordered page header is invalid");
    }
    const auto count = readLittleEndian<std::uint32_t>(payload, PageLayout::entryCount);
    const auto prefixLength = readLittleEndian<std::uint32_t>(payload, PageLayout::prefixLength);
    const auto slotsOffset = readLittleEndian<std::uint32_t>(payload, PageLayout::slotsOffset);
    const auto entriesOffset = readLittleEndian<std::uint32_t>(payload, PageLayout::entriesOffset);
    const auto usedLength = readLittleEndian<std::uint32_t>(payload, PageLayout::usedLength);
    if ((type == 1 && count == 0) || slotsOffset != PageLayout::bytes + prefixLength ||
        entriesOffset != slotsOffset + static_cast<std::uint64_t>(count) * 8 ||
        usedLength < entriesOffset || usedLength > payload.size() ||
        !allZero(payload, usedLength, payload.size()) ||
        (type == 1 && !allZero(payload, PageLayout::leftmostChild, PageLayout::reserved))) {
        throwCorrupt("ordered page bounds are invalid");
    }
    const auto prefix = ByteView{payload}.subspan(PageLayout::bytes, prefixLength);
    std::size_t expectedEntry = entriesOffset;
    StoredBytes<Allocator> firstKey{ByteAllocator{allocator}};
    using Child = std::pair<StoredBytes<Allocator>, ExtentReference>;
    StoredVector<Child, Allocator> children{
        typename std::allocator_traits<Allocator>::
            template rebind_alloc<Child>{allocator}};
    if (type == 2) {
        children.emplace_back(
            StoredBytes<Allocator>{ByteAllocator{allocator}},
            decodeExtentReference(ByteView{payload}.subspan(PageLayout::leftmostChild, 32)));
    }
    StoredBytes<Allocator> previousKey{ByteAllocator{allocator}};
    for (std::uint32_t index = 0; index != count; ++index) {
        const auto slot = slotsOffset + index * 8U;
        const auto entryOffset = readLittleEndian<std::uint32_t>(payload, slot);
        const auto entryLength = readLittleEndian<std::uint32_t>(payload, slot + 4);
        if (entryOffset != expectedEntry || entryLength < (type == 1 ? 20U : 36U) ||
            entryOffset > usedLength || entryLength > usedLength - entryOffset) {
            throwCorrupt("ordered page slot is invalid");
        }
        const auto suffixLength = readLittleEndian<std::uint32_t>(payload, entryOffset);
        const auto fixedLength = type == 1 ? 20U : 36U;
        if (suffixLength > entryLength - fixedLength) {
            throwCorrupt("ordered page key suffix is invalid");
        }
        StoredBytes<Allocator> key{ByteAllocator{allocator}};
        key.assign(prefix.begin(), prefix.end());
        key.insert(key.end(), payload.begin() + entryOffset + 4,
                   payload.begin() + entryOffset + 4 + suffixLength);
        if (key.size() > Limits::maxKeyBytes ||
            (index != 0 && !UnsignedBytesLess{}(previousKey, key))) {
            throwCorrupt("ordered page keys are not canonical");
        }
        if (index == 0) {
            firstKey = key;
        }
        previousKey = key;
        if (type == 1) {
            const auto representation = entryOffset + 4 + suffixLength;
            if ((payload[representation] != std::byte{0} && payload[representation] != std::byte{1}) ||
                !allZero(payload, representation + 1, representation + 8)) {
                throwCorrupt("ordered leaf value representation is invalid");
            }
            const auto valueLength = readLittleEndian<std::uint64_t>(payload, representation + 8);
            const bool overflow = payload[representation] == std::byte{1};
            if (valueLength > Limits::maxValueBytes ||
                overflow != (valueLength > Limits::maxInlineValueBytes) ||
                entryLength != 20ULL + suffixLength + (overflow ? 32ULL : valueLength)) {
                throwCorrupt("ordered leaf value length is invalid");
            }
            StoredBytes<Allocator> value{ByteAllocator{allocator}};
            ExtentReference overflowReference;
            if (overflow) {
                overflowReference = decodeExtentReference(
                    ByteView{payload}.subspan(representation + 16, 32));
                if (reachable) {
                    reachable->push_back(overflowReference);
                }
                value = readAuthenticatedExtent<Limits>(
                    file, overflowReference, 11, valueLength,
                    opened, providers, allocator);
            } else {
                value.assign(payload.begin() + representation + 16,
                             payload.begin() + representation + 16 + valueLength);
            }
            if (mutableNode) {
                mutableNode->values.push_back(MutableTreeValue<Allocator>{
                    key, value, overflowReference});
            }
            if (!values.emplace(std::move(key), std::move(value)).second) {
                throwCorrupt("ordered leaf key is duplicated");
            }
        } else {
            children.emplace_back(
                std::move(key),
                decodeExtentReference(ByteView{payload}.subspan(
                    entryOffset + 4 + suffixLength, 32)));
        }
        expectedEntry += entryLength;
    }
    if (expectedEntry != usedLength ||
        commonPrefixLength(firstKey, previousKey) != prefixLength) {
        throwCorrupt("ordered page image is noncanonical");
    }
    if (type == 1) {
        if (mutableNode) {
            mutableNode->minimumKey = firstKey;
        }
        return OrderedPageBounds<Allocator>{
            std::move(firstKey), std::move(previousKey)};
    }
    StoredBytes<Allocator> subtreeMinimum{ByteAllocator{allocator}};
    StoredBytes<Allocator> subtreeMaximum{ByteAllocator{allocator}};
    if (mutableNode) {
        mutableNode->children.reserve(children.size());
        for (std::size_t index = 0; index != children.size(); ++index) {
            mutableNode->children.emplace_back(allocator);
        }
    }
    for (std::size_t index = 0; index != children.size(); ++index) {
        auto childBounds = loadOrderedPage<Limits>(
            file, children[index].second, level - 1,
            opened, providers, allocator, values, reachable,
            mutableNode ? &mutableNode->children[index] : nullptr);
        if (index == 0) {
            subtreeMinimum = std::move(childBounds.minimum);
        } else {
            if (childBounds.minimum != children[index].first) {
                throwCorrupt(
                    "ordered internal separator is not the right-subtree minimum");
            }
            if (!UnsignedBytesLess{}(subtreeMaximum, childBounds.minimum)) {
                throwCorrupt("ordered subtrees overlap or are reordered");
            }
        }
        subtreeMaximum = std::move(childBounds.maximum);
    }
    if (mutableNode) {
        mutableNode->minimumKey = subtreeMinimum;
    }
    return OrderedPageBounds<Allocator>{
        std::move(subtreeMinimum), std::move(subtreeMaximum)};
}

template<class Limits, class Allocator>
[[nodiscard]] inline std::optional<std::uint32_t> shallowValidateOrderedRoot(
    DurableFile& file,
    OpenedDatabase& opened,
    ProviderSet& providers,
    const Allocator& allocator) {
    const auto reference = decodeExtentReference(opened.format.orderedRoot);
    if (reference.null()) {
        return std::nullopt;
    }
    validateOrderedPageReference<Limits>(
        reference, opened.format.generation, opened.format.highWaterBytes);
    std::array<std::byte, ExtentLayout::bytes> preamble{};
    file.readExactAt(
        reference.blockIndex * Limits::allocationQuantumBytes,
        preamble);
    const auto kind = readLittleEndian<std::uint16_t>(
        preamble, ExtentLayout::unitKind);
    if (kind != 1 && kind != 2) {
        throwCorrupt("ordered root role is invalid");
    }
    const auto payload = readAuthenticatedExtent<Limits>(
        file,
        reference,
        kind,
        std::nullopt,
        opened,
        providers,
        allocator);
    const auto type = readLittleEndian<std::uint16_t>(
        payload, PageLayout::type);
    const auto level = readLittleEndian<std::uint32_t>(
        payload, PageLayout::level);
    const auto count = readLittleEndian<std::uint32_t>(
        payload, PageLayout::entryCount);
    const auto prefixLength = readLittleEndian<std::uint32_t>(
        payload, PageLayout::prefixLength);
    const auto slotsOffset = readLittleEndian<std::uint32_t>(
        payload, PageLayout::slotsOffset);
    const auto entriesOffset = readLittleEndian<std::uint32_t>(
        payload, PageLayout::entriesOffset);
    const auto usedLength = readLittleEndian<std::uint32_t>(
        payload, PageLayout::usedLength);
    if (!matches(payload, PageLayout::magic, "MIAREPG\0") ||
        readLittleEndian<std::uint16_t>(payload, PageLayout::version) != 1 ||
        (type != 1 && type != 2) || kind != (type == 1 ? 2 : 1) ||
        readLittleEndian<std::uint32_t>(
            payload, PageLayout::headerLength) != PageLayout::bytes ||
        readLittleEndian<std::uint32_t>(payload, PageLayout::role) != 1 ||
        level > maximumTreeLevel || (type == 1) != (level == 0) ||
        (type == 1 && count == 0) || (type == 2 && count == 0) ||
        readLittleEndian<std::uint32_t>(payload, PageLayout::flags) != 0 ||
        slotsOffset != PageLayout::bytes + prefixLength ||
        entriesOffset != slotsOffset + static_cast<std::uint64_t>(count) * 8 ||
        usedLength < entriesOffset || usedLength > payload.size() ||
        !allZero(payload, PageLayout::reserved, PageLayout::bytes) ||
        (type == 1 &&
         !allZero(payload, PageLayout::leftmostChild, PageLayout::reserved))) {
        throwCorrupt("ordered root header is invalid");
    }
    if (type == 2) {
        const auto leftmost = decodeExtentReference(
            ByteView{payload}.subspan(PageLayout::leftmostChild, 32));
        if (leftmost.null()) {
            throwCorrupt("ordered internal root has a null child");
        }
        validateOrderedPageReference<Limits>(
            leftmost,
            opened.format.generation,
            opened.format.highWaterBytes);
    }
    return level;
}

template<class Limits, class Allocator>
[[nodiscard]] inline OrderedKeyValues<Allocator> loadExactValues(
    DurableFile& file,
    OpenedDatabase& opened,
    ProviderSet& providers,
    const Allocator& allocator,
    ExtentReferences<Allocator>* reachable = nullptr,
    MutableTreeNode<Allocator>* mutableRoot = nullptr) {
    auto values = makeOrderedKeyValues(allocator);
    const auto reference = decodeExtentReference(opened.format.orderedRoot);
    const auto level = shallowValidateOrderedRoot<Limits>(
        file, opened, providers, allocator);
    if (!level) {
        return values;
    }
    (void)loadOrderedPage<Limits>(
        file, reference, *level, opened, providers, allocator, values, reachable,
        mutableRoot);
    return values;
}

template<class Allocator>
struct FixedTreeBounds {
    StoredBytes<Allocator> minimum;
    StoredBytes<Allocator> maximum;
};

[[nodiscard]] inline bool fixedTreeKeyLess(
    ByteView left,
    ByteView right,
    std::uint32_t role) {
    return role == 3
        ? readLittleEndian<std::uint64_t>(left, 0) <
              readLittleEndian<std::uint64_t>(right, 0)
        : UnsignedBytesLess{}(left, right);
}

template<class Limits, class Allocator>
[[nodiscard]] inline FixedTreeBounds<Allocator> loadFixedTreePage(
    DurableFile& file,
    const ExtentReference& reference,
    std::uint32_t expectedLevel,
    std::uint32_t role,
    std::size_t keySize,
    std::uint16_t internalKind,
    std::uint16_t leafKind,
    std::uint32_t keyDomain,
    ByteView owner,
    OpenedDatabase& opened,
    ProviderSet& providers,
    const Allocator& allocator,
    StoredVector<FixedLeafEntry<Allocator>, Allocator>& entries,
    ExtentReferences<Allocator>* reachable) {
    if (expectedLevel > maximumTreeLevel) {
        throwCorrupt("fixed-key tree exceeds the supported depth");
    }
    using ByteAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<std::byte>;
    validateOrderedPageReference<Limits>(
        reference, opened.format.generation, opened.format.highWaterBytes);
    if (reachable) {
        reachable->push_back(reference);
    }
    std::array<std::byte, ExtentLayout::bytes> preamble{};
    file.readExactAt(
        reference.blockIndex * Limits::allocationQuantumBytes, preamble);
    const auto kind = readLittleEndian<std::uint16_t>(
        preamble, ExtentLayout::unitKind);
    if (kind != internalKind && kind != leafKind) {
        throwCorrupt("fixed-key page role is invalid");
    }
    auto payload = readAuthenticatedExtent<Limits>(
        file, reference, kind, std::nullopt, opened, providers, allocator,
        true, keyDomain, 0, owner);
    const auto type = readLittleEndian<std::uint16_t>(
        payload, PageLayout::type);
    const auto level = readLittleEndian<std::uint32_t>(
        payload, PageLayout::level);
    if (!matches(payload, PageLayout::magic, "MIAREPG\0") ||
        readLittleEndian<std::uint16_t>(payload, PageLayout::version) != 1 ||
        (type != 1 && type != 2) ||
        kind != (type == 1 ? leafKind : internalKind) ||
        readLittleEndian<std::uint32_t>(
            payload, PageLayout::headerLength) != PageLayout::bytes ||
        readLittleEndian<std::uint32_t>(payload, PageLayout::role) != role ||
        level != expectedLevel || (type == 1) != (level == 0) ||
        readLittleEndian<std::uint32_t>(payload, PageLayout::flags) != 0 ||
        !allZero(payload, PageLayout::reserved, PageLayout::bytes)) {
        throwCorrupt("fixed-key page header is invalid");
    }
    const auto count = readLittleEndian<std::uint32_t>(
        payload, PageLayout::entryCount);
    const auto prefixLength = readLittleEndian<std::uint32_t>(
        payload, PageLayout::prefixLength);
    const auto slotsOffset = readLittleEndian<std::uint32_t>(
        payload, PageLayout::slotsOffset);
    const auto entriesOffset = readLittleEndian<std::uint32_t>(
        payload, PageLayout::entriesOffset);
    const auto usedLength = readLittleEndian<std::uint32_t>(
        payload, PageLayout::usedLength);
    if ((type == 1 && count == 0) || prefixLength > keySize ||
        slotsOffset != PageLayout::bytes + prefixLength ||
        entriesOffset != slotsOffset + static_cast<std::uint64_t>(count) * 8 ||
        usedLength < entriesOffset || usedLength > payload.size() ||
        !allZero(payload, usedLength, payload.size()) ||
        (type == 1 &&
         !allZero(payload, PageLayout::leftmostChild, PageLayout::reserved))) {
        throwCorrupt("fixed-key page bounds are invalid");
    }
    const auto prefix = ByteView{payload}.subspan(
        PageLayout::bytes, prefixLength);
    using Child = std::pair<StoredBytes<Allocator>, ExtentReference>;
    StoredVector<Child, Allocator> children{
        typename std::allocator_traits<Allocator>::template rebind_alloc<Child>{
            allocator}};
    if (type == 2) {
        const auto leftmost = decodeExtentReference(
            ByteView{payload}.subspan(PageLayout::leftmostChild, 32));
        if (leftmost.null()) {
            throwCorrupt("fixed-key internal page has a null child");
        }
        children.emplace_back(
            StoredBytes<Allocator>{ByteAllocator{allocator}}, leftmost);
    }
    StoredBytes<Allocator> firstKey{ByteAllocator{allocator}};
    StoredBytes<Allocator> previousKey{ByteAllocator{allocator}};
    std::size_t expectedEntry = entriesOffset;
    for (std::uint32_t index = 0; index != count; ++index) {
        const auto slot = slotsOffset + index * 8U;
        const auto entryOffset = readLittleEndian<std::uint32_t>(payload, slot);
        const auto entryLength = readLittleEndian<std::uint32_t>(
            payload, slot + 4);
        if (entryOffset != expectedEntry || entryLength < 36 ||
            entryOffset > usedLength || entryLength > usedLength - entryOffset) {
            throwCorrupt("fixed-key page slot is invalid");
        }
        const auto suffixLength = readLittleEndian<std::uint32_t>(
            payload, entryOffset);
        if (entryLength != 36ULL + suffixLength ||
            prefixLength + suffixLength != keySize) {
            throwCorrupt("fixed-key page entry is invalid");
        }
        StoredBytes<Allocator> key{ByteAllocator{allocator}};
        key.assign(prefix.begin(), prefix.end());
        key.insert(
            key.end(),
            payload.begin() + entryOffset + 4,
            payload.begin() + entryOffset + 4 + suffixLength);
        if (index != 0 && !fixedTreeKeyLess(previousKey, key, role)) {
            throwCorrupt("fixed-key page keys are not canonical");
        }
        if (index == 0) {
            firstKey = key;
        }
        previousKey = key;
        const auto child = decodeExtentReference(ByteView{payload}.subspan(
            entryOffset + 4 + suffixLength, 32));
        if (child.null()) {
            throwCorrupt("fixed-key page has a null reference");
        }
        if (type == 1) {
            validateExtentReference<Limits>(
                child, opened.format.generation, opened.format.highWaterBytes);
            entries.push_back(FixedLeafEntry<Allocator>{
                std::move(key), child});
            if (reachable) {
                reachable->push_back(child);
            }
        } else {
            children.emplace_back(std::move(key), child);
        }
        expectedEntry += entryLength;
    }
    if (expectedEntry != usedLength ||
        commonPrefixLength(firstKey, previousKey) != prefixLength) {
        throwCorrupt("fixed-key page image is noncanonical");
    }
    if (type == 1) {
        return FixedTreeBounds<Allocator>{
            std::move(firstKey), std::move(previousKey)};
    }
    StoredBytes<Allocator> subtreeMinimum{ByteAllocator{allocator}};
    StoredBytes<Allocator> subtreeMaximum{ByteAllocator{allocator}};
    for (std::size_t index = 0; index != children.size(); ++index) {
        auto bounds = loadFixedTreePage<Limits>(
            file, children[index].second, level - 1, role, keySize,
            internalKind, leafKind, keyDomain, owner, opened, providers,
            allocator, entries, reachable);
        if (index == 0) {
            subtreeMinimum = std::move(bounds.minimum);
        } else if (children[index].first != bounds.minimum ||
                   !fixedTreeKeyLess(subtreeMaximum, bounds.minimum, role)) {
            throwCorrupt("fixed-key internal separator is invalid");
        }
        subtreeMaximum = std::move(bounds.maximum);
    }
    return FixedTreeBounds<Allocator>{
        std::move(subtreeMinimum), std::move(subtreeMaximum)};
}

template<class Limits, class Allocator>
[[nodiscard]] inline StoredVector<FixedLeafEntry<Allocator>, Allocator>
loadFixedTree(
    DurableFile& file,
    const ExtentReference& root,
    std::uint32_t role,
    std::size_t keySize,
    std::uint16_t internalKind,
    std::uint16_t leafKind,
    std::uint32_t keyDomain,
    ByteView owner,
    OpenedDatabase& opened,
    ProviderSet& providers,
    const Allocator& allocator,
    ExtentReferences<Allocator>* reachable = nullptr) {
    using Entry = FixedLeafEntry<Allocator>;
    StoredVector<Entry, Allocator> entries{
        typename std::allocator_traits<Allocator>::template rebind_alloc<Entry>{
            allocator}};
    if (root.null()) {
        return entries;
    }
    std::array<std::byte, ExtentLayout::bytes> preamble{};
    file.readExactAt(
        root.blockIndex * Limits::allocationQuantumBytes, preamble);
    const auto kind = readLittleEndian<std::uint16_t>(
        preamble, ExtentLayout::unitKind);
    if (kind != internalKind && kind != leafKind) {
        throwCorrupt("fixed-key root role is invalid");
    }
    auto payload = readAuthenticatedExtent<Limits>(
        file, root, kind, std::nullopt, opened, providers, allocator,
        true, keyDomain, 0, owner);
    const auto level = readLittleEndian<std::uint32_t>(
        payload, PageLayout::level);
    if (readLittleEndian<std::uint16_t>(payload, PageLayout::type) == 2 &&
        readLittleEndian<std::uint32_t>(payload, PageLayout::entryCount) == 0) {
        throwCorrupt("fixed-key root has only one child");
    }
    (void)loadFixedTreePage<Limits>(
        file, root, level, role, keySize, internalKind, leafKind, keyDomain,
        owner, opened, providers, allocator, entries, reachable);
    return entries;
}

template<class Limits, class Allocator>
[[nodiscard]] inline BlobCatalog<Allocator> loadBlobCatalog(
    DurableFile& file,
    OpenedDatabase& opened,
    ProviderSet& providers,
    const Allocator& allocator,
    ExtentReferences<Allocator>* reachable = nullptr) {
    auto blobs = makeBlobCatalog(allocator);
    const auto root = decodeExtentReference(opened.format.blobRoot);
    if (opened.format.generation == 1) {
        if (!root.null()) {
            throwCorrupt("initial database has a Blob catalog");
        }
        return blobs;
    }
    auto catalogEntries = loadFixedTree<Limits>(
        file, root, 2, BlobId::encodedSize, 3, 4, 2, {},
        opened, providers, allocator, reachable);
    using ReferenceAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<ExtentReference>;
    using VersionAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<BlobVersion<Allocator>>;
    for (const auto& catalogEntry : catalogEntries) {
        std::array<std::byte, BlobId::encodedSize> idBytes{};
        std::copy(
            catalogEntry.key.begin(), catalogEntry.key.end(), idBytes.begin());
        const auto id = BlobId::fromBytes(idBytes);
        const auto owner = id.toBytes();
        auto manifest = readAuthenticatedExtent<Limits>(
            file,
            catalogEntry.reference,
            12,
            BlobManifestLayout::bytes,
            opened,
            providers,
            allocator,
            false,
            4,
            0,
            owner);
        const auto contentGeneration = readLittleEndian<std::uint64_t>(
            manifest, BlobManifestLayout::generation);
        const auto logicalLength = readLittleEndian<std::uint64_t>(
            manifest, BlobManifestLayout::logicalLength);
        const auto chunkCount = readLittleEndian<std::uint64_t>(
            manifest, BlobManifestLayout::chunkCount);
        const auto chunkRoot = decodeExtentReference(ByteView{manifest}.subspan(
            BlobManifestLayout::chunkRoot, 32));
        const auto expectedChunkCount = logicalLength == 0
            ? 0
            : 1 + (logicalLength - 1) / Limits::blobChunkBytes;
        if (!matches(manifest, BlobManifestLayout::magic, "MIAREBLB") ||
            readLittleEndian<std::uint16_t>(
                manifest, BlobManifestLayout::version) != 1 ||
            readLittleEndian<std::uint16_t>(
                manifest, BlobManifestLayout::flags) != 0 ||
            readLittleEndian<std::uint32_t>(
                manifest, BlobManifestLayout::length) !=
                BlobManifestLayout::bytes ||
            !std::equal(
                owner.begin(),
                owner.end(),
                manifest.begin() + BlobManifestLayout::id) ||
            contentGeneration != catalogEntry.reference.creationGeneration ||
            logicalLength > Limits::maxBlobBytes ||
            readLittleEndian<std::uint64_t>(
                manifest, BlobManifestLayout::chunkSize) !=
                Limits::blobChunkBytes ||
            chunkCount != expectedChunkCount ||
            chunkRoot.null() != (chunkCount == 0) ||
            !allZero(
                manifest,
                BlobManifestLayout::reserved,
                BlobManifestLayout::bytes)) {
            throwCorrupt("Blob manifest is noncanonical");
        }
        ExtentReferences<Allocator> versionReachable{
            ReferenceAllocator{allocator}};
        versionReachable.push_back(catalogEntry.reference);
        auto chunkEntries = loadFixedTree<Limits>(
            file, chunkRoot, 3, 8, 5, 6, 4, owner,
            opened, providers, allocator, &versionReachable);
        if (chunkEntries.size() != chunkCount) {
            throwCorrupt("Blob chunk index is not dense");
        }
        if (std::any_of(
                versionReachable.begin() + 1,
                versionReachable.end(),
                [&](const auto& reference) {
                    return reference.creationGeneration != contentGeneration;
                })) {
            throwCorrupt("Blob content version generations disagree");
        }
        ExtentReferences<Allocator> chunks{ReferenceAllocator{allocator}};
        chunks.reserve(chunkEntries.size());
        for (std::uint64_t ordinal = 0; ordinal != chunkCount; ++ordinal) {
            if (readLittleEndian<std::uint64_t>(
                    chunkEntries[ordinal].key, 0) != ordinal ||
                chunkEntries[ordinal].reference.creationGeneration !=
                    contentGeneration) {
                throwCorrupt("Blob chunk index is noncanonical");
            }
            chunks.push_back(chunkEntries[ordinal].reference);
        }
        if (reachable) {
            reachable->insert(
                reachable->end(),
                versionReachable.begin() + 1,
                versionReachable.end());
        }
        auto version = std::allocate_shared<BlobVersion<Allocator>>(
            VersionAllocator{allocator},
            id,
            logicalLength,
            contentGeneration,
            catalogEntry.reference,
            chunkRoot,
            std::move(chunks),
            std::move(versionReachable));
        if (!blobs.emplace(id, std::move(version)).second) {
            throwCorrupt("Blob catalog identifier is duplicated");
        }
    }
    return blobs;
}

template<class Allocator>
inline void coalesceBlobStagingFreeRuns(
    ExtentRuns<Allocator>& runs,
    const Allocator& allocator) {
    using RunAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<ExtentRun>;
    std::sort(
        runs.begin(), runs.end(),
        [](const auto& left, const auto& right) {
            return left.start < right.start;
        });
    ExtentRuns<Allocator> coalesced{RunAllocator{allocator}};
    for (const auto& run : runs) {
        if (run.count == 0) {
            continue;
        }
        if (!coalesced.empty() &&
            coalesced.back().start + coalesced.back().count == run.start) {
            coalesced.back().count += run.count;
        } else {
            coalesced.push_back(run);
        }
    }
    runs = std::move(coalesced);
}

template<class Allocator>
inline void insertBlobStagingFreeRun(
    ExtentRuns<Allocator>& runs,
    ExtentRun inserted) {
    auto position = std::lower_bound(
        runs.begin(), runs.end(), inserted.start,
        [](const auto& run, std::uint64_t start) {
            return run.start < start;
        });
    if (position != runs.begin()) {
        auto previous = position - 1;
        if (previous->start + previous->count >= inserted.start) {
            const auto end = std::max(
                previous->start + previous->count,
                inserted.start + inserted.count);
            previous->count = end - previous->start;
            position = previous;
        } else {
            position = runs.insert(position, inserted);
        }
    } else {
        position = runs.insert(position, inserted);
    }
    while (position + 1 != runs.end() &&
           position->start + position->count >= (position + 1)->start) {
        const auto following = position + 1;
        const auto end = std::max(
            position->start + position->count,
            following->start + following->count);
        position->count = end - position->start;
        runs.erase(following);
    }
}

template<class Limits, class Allocator>
inline void initializeBlobStagingAllocator(
    BlobWriteState<Allocator, Limits>& state) {
    if (state.allocatorInitialized) {
        return;
    }
    auto& session = *state.session;
    using ReferenceAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<ExtentReference>;
    using RunAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<ExtentRun>;
    ExtentReferences<Allocator> reachable{
        ReferenceAllocator{session.allocator}};
    ExtentRuns<Allocator> freeRuns{RunAllocator{session.allocator}};
    ExtentRuns<Allocator> retiredRuns{RunAllocator{session.allocator}};
    (void)loadExactValues<Limits>(
        *session.file,
        session.opened,
        *session.providers,
        session.allocator,
        &reachable);
    (void)loadBlobCatalog<Limits>(
        *session.file,
        session.opened,
        *session.providers,
        session.allocator,
        &reachable);
    loadAllocatorReferences<Limits>(
        *session.file,
        session.opened,
        *session.providers,
        session.allocator,
        reachable,
        &freeRuns,
        &retiredRuns);
    std::uint64_t oldestReaderGeneration =
        std::numeric_limits<std::uint64_t>::max();
    {
        std::lock_guard lock{session.mutex};
        for (const auto& [identity, reader] : session.activeReaders) {
            (void)identity;
            oldestReaderGeneration = std::min(
                oldestReaderGeneration, reader.generation);
        }
    }
    for (const auto& run : retiredRuns) {
        if (oldestReaderGeneration >= run.retirementGeneration) {
            freeRuns.push_back(ExtentRun{
                run.start, run.count});
        }
    }
    coalesceBlobStagingFreeRuns(
        freeRuns, session.allocator);
    state.stagingFreeRuns = std::move(freeRuns);
    state.stagingNextBlock =
        session.opened.format.highWaterBytes /
        Limits::allocationQuantumBytes;
    state.allocatorInitialized = true;
}

template<class Limits, class Allocator>
inline void releaseBlobStagingReference(
    BlobWriteState<Allocator, Limits>& state,
    const ExtentReference& reference) noexcept {
    assert(state.abortableStagingReferences != 0);
    assert(state.stagingFreeRuns.capacity() > state.stagingFreeRuns.size());
    insertBlobStagingFreeRun<Allocator>(
        state.stagingFreeRuns,
        ExtentRun{reference.blockIndex, reference.blockCount});
    --state.abortableStagingReferences;
}

template<class Limits, class Allocator, class References>
inline void releaseBlobStagingReferences(
    BlobWriteState<Allocator, Limits>& state,
    const References& references) noexcept {
    for (const auto& reference : references) {
        releaseBlobStagingReference<Limits>(state, reference);
    }
}

template<class Limits, class Allocator>
[[nodiscard]] inline ExtentReference stageBlobChunk(
    BlobWriteState<Allocator, Limits>& state,
    BlobId id,
    std::uint64_t ordinal,
    ByteView decoded) {
    initializeBlobStagingAllocator<Limits>(state);
    auto& session = *state.session;
    const auto owner = id.toBytes();
    const auto generation = session.opened.format.generation + 1;
    auto prepared = prepareAuthenticatedExtent<Limits>(
        decoded,
        13,
        generation,
        state.stagingNextBlock,
        session.opened,
        *session.providers,
        session.allocator,
        true,
        4,
        ordinal,
        owner);
    state.stagingFreeRuns.reserve(
        state.stagingFreeRuns.size() +
        state.abortableStagingReferences + 1);
    auto allocated = state.stagingNextBlock;
    bool appended = true;
    for (auto& run : state.stagingFreeRuns) {
        if (run.count >= prepared.reference.blockCount) {
            allocated = run.start;
            run.start += prepared.reference.blockCount;
            run.count -= prepared.reference.blockCount;
            appended = false;
            break;
        }
    }
    if (appended) {
        state.stagingNextBlock += prepared.reference.blockCount;
    }
    const ExtentReference reserved{
        allocated,
        prepared.reference.blockCount,
        prepared.reference.encodedLength,
        generation};
    try {
        if (allocated != prepared.reference.blockIndex) {
            prepared = prepareAuthenticatedExtent<Limits>(
                decoded,
                13,
                generation,
                allocated,
                session.opened,
                *session.providers,
                session.allocator,
                true,
                4,
                ordinal,
                owner);
        }
        if (allocated > std::numeric_limits<std::uint64_t>::max() -
                prepared.reference.blockCount) {
            throw DatabaseError{
                Errc::ResourceLimit,
                "Blob staging block range is not representable"};
        }
        const auto requiredBlocks =
            allocated + prepared.reference.blockCount;
        if (requiredBlocks > std::numeric_limits<std::uint64_t>::max() /
                Limits::allocationQuantumBytes) {
            throw DatabaseError{
                Errc::ResourceLimit,
                "Blob staging byte range is not representable"};
        }
        const auto requiredBytes =
            requiredBlocks * Limits::allocationQuantumBytes;
        if (requiredBytes > Limits::maxDatabaseBytes ||
            (requiredBytes > session.opened.format.highWaterBytes &&
             requiredBytes - session.opened.format.highWaterBytes >
                 Limits::maxFileGrowthPerTransaction)) {
            session.capacityFailureCount.fetch_add(
                1, std::memory_order_relaxed);
            throw DatabaseError{
                Errc::ResourceLimit,
                "Blob staging exceeds the database capacity profile"};
        }
        if (requiredBytes > session.file->size()) {
            session.file->resize(requiredBytes);
        }
        session.file->writeExactAt(
            allocated * Limits::allocationQuantumBytes,
            prepared.bytes);
        {
            std::lock_guard lock{session.mutex};
            const auto physicalBytes = session.file->size();
            session.opened.abandonedTailBytes =
                physicalBytes > session.opened.format.highWaterBytes
                ? physicalBytes - session.opened.format.highWaterBytes
                : 0;
        }
        ++state.abortableStagingReferences;
        return prepared.reference;
    } catch (...) {
        insertBlobStagingFreeRun<Allocator>(
            state.stagingFreeRuns,
            ExtentRun{reserved.blockIndex, reserved.blockCount});
        throw;
    }
}

template<class Limits, class Allocator>
inline void readBlobRange(
    DatabaseSession<Allocator, Limits>& session,
    const BlobVersion<Allocator>& version,
    std::uint64_t offset,
    MutableByteView destination) {
    const auto owner = version.id.toBytes();
    std::size_t copied = 0;
    while (copied != destination.size()) {
        const auto absolute = offset + copied;
        const auto ordinal = absolute / Limits::blobChunkBytes;
        const auto within = absolute % Limits::blobChunkBytes;
        const auto expectedLength = ordinal + 1 == version.chunks.size()
            ? version.size - ordinal * Limits::blobChunkBytes
            : Limits::blobChunkBytes;
        if (expectedLength > session.cacheCapacityBytes) {
            session.capacityFailureCount.fetch_add(
                1, std::memory_order_relaxed);
            throw DatabaseError{
                Errc::ResourceLimit,
                "Blob chunk exceeds the configured cache capacity"};
        }
        StoredBytes<Allocator> chunk{
            typename std::allocator_traits<Allocator>::template rebind_alloc<
                std::byte>{session.allocator}};
        {
            std::lock_guard lock{session.mutex};
            chunk = readAuthenticatedExtent<Limits>(
                *session.file,
                version.chunks[ordinal],
                13,
                expectedLength,
                session.opened,
                *session.providers,
                session.allocator,
                true,
                4,
                ordinal,
                owner,
                version.pending
                    ? std::optional<std::uint64_t>{version.generation}
                    : std::nullopt,
                version.pending
                    ? std::optional<std::uint64_t>{version.stagedHighWaterBytes}
                    : std::nullopt);
        }
        const auto count = std::min<std::size_t>(
            destination.size() - copied,
            chunk.size() - static_cast<std::size_t>(within));
        std::copy_n(
            chunk.begin() + static_cast<std::ptrdiff_t>(within),
            count,
            destination.begin() + static_cast<std::ptrdiff_t>(copied));
        copied += count;
    }
}

template<class Allocator>
[[nodiscard]] inline std::size_t mutableChildFor(
    const MutableTreeNode<Allocator>& node,
    ByteView key) {
    std::size_t child = 0;
    while (child + 1 != node.children.size() &&
           !UnsignedBytesLess{}(key, node.children[child + 1].minimumKey)) {
        ++child;
    }
    return child;
}

template<class Allocator, class UsedLength>
[[nodiscard]] inline StoredVector<std::pair<std::size_t, std::size_t>, Allocator>
balancedPageRanges(
    std::size_t count,
    std::uint64_t pagePayloadBytes,
    const Allocator& allocator,
    UsedLength&& usedLength) {
    using Range = std::pair<std::size_t, std::size_t>;
    StoredVector<Range, Allocator> ranges{
        typename std::allocator_traits<Allocator>::
            template rebind_alloc<Range>{allocator}};
    std::size_t begin = 0;
    auto scan = begin + 1;
    while (scan != count) {
        if (usedLength(begin, scan + 1) <= pagePayloadBytes) {
            ++scan;
            continue;
        }
        std::size_t selected = 0;
        auto selectedDifference = std::numeric_limits<std::uint64_t>::max();
        for (auto boundary = begin + 1; boundary != scan + 1; ++boundary) {
            const auto left = usedLength(begin, boundary);
            const auto right = usedLength(boundary, scan + 1);
            if (left > pagePayloadBytes || right > pagePayloadBytes) {
                continue;
            }
            const auto difference = left > right ? left - right : right - left;
            if (difference < selectedDifference) {
                selected = boundary;
                selectedDifference = difference;
            }
        }
        if (selected == 0) {
            throw DatabaseError{
                Errc::ResourceLimit,
                "page overflow has no valid split boundary"};
        }
        ranges.emplace_back(begin, selected);
        begin = selected;
        ++scan;
    }
    ranges.emplace_back(begin, count);
    return ranges;
}

template<class Limits, class Allocator, class Entries, class PrepareExtent>
[[nodiscard]] inline ExtentReference persistFixedTree(
    const Entries& entries,
    std::uint32_t role,
    std::uint16_t internalKind,
    std::uint16_t leafKind,
    const Allocator& allocator,
    PrepareExtent&& prepareExtent,
    StoredVector<PreparedExactExtent<Allocator>, Allocator>& extents) {
    if (entries.empty()) {
        return {};
    }
    using Node = PreparedTreeNode<Allocator>;
    using NodeAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<Node>;
    constexpr auto pagePayloadBytes = std::max<std::uint64_t>(
        16U * 1024U, Limits::allocationQuantumBytes) -
        ExtentLayout::bytes - authenticationTagBytes;
    auto ranges = balancedPageRanges(
        entries.size(),
        pagePayloadBytes,
        allocator,
        [&](std::size_t begin, std::size_t end) {
            return fixedLeafUsedLength(entries, begin, end);
        });
    StoredVector<Node, Allocator> nodes{NodeAllocator{allocator}};
    nodes.reserve(ranges.size());
    for (const auto [begin, end] : ranges) {
        auto payload = encodeFixedLeafPage<Limits>(
            entries, begin, end, role, allocator);
        auto prepared = prepareExtent(payload, leafKind);
        nodes.push_back(Node{
            prepared.reference,
            entries[begin].key,
            0});
        extents.push_back(std::move(prepared));
    }
    while (nodes.size() != 1) {
        ranges = balancedPageRanges(
            nodes.size(),
            pagePayloadBytes,
            allocator,
            [&](std::size_t begin, std::size_t end) {
                return internalUsedLength<Allocator>(nodes, begin, end);
            });
        StoredVector<Node, Allocator> parents{NodeAllocator{allocator}};
        parents.reserve(ranges.size());
        for (const auto [begin, end] : ranges) {
            auto payload = encodeInternalPage<Limits>(
                nodes, begin, end, allocator, role);
            auto prepared = prepareExtent(payload, internalKind);
            parents.push_back(Node{
                prepared.reference,
                nodes[begin].minimumKey,
                nodes[begin].level + 1});
            extents.push_back(std::move(prepared));
        }
        nodes = std::move(parents);
    }
    return nodes.front().reference;
}

template<class Limits, class Allocator, class Runs, class PrepareExtent>
[[nodiscard]] inline ExtentReference persistAllocatorIndex(
    const Runs& runs,
    bool retired,
    const Allocator& allocator,
    PrepareExtent&& prepareExtent,
    StoredVector<PreparedExactExtent<Allocator>, Allocator>& extents) {
    if (runs.empty()) {
        return {};
    }
    using Node = PreparedTreeNode<Allocator>;
    using NodeAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<Node>;
    using ByteAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<std::byte>;
    constexpr auto pagePayloadBytes = std::max<std::uint64_t>(
        16U * 1024U, Limits::allocationQuantumBytes) -
        ExtentLayout::bytes - authenticationTagBytes;
    const auto leafKind = static_cast<std::uint16_t>(retired ? 10 : 8);
    const auto internalKind = static_cast<std::uint16_t>(retired ? 9 : 7);
    const auto role = retired ? 5U : 4U;
    const auto keyLength = retired ? 16U : 8U;
    auto ranges = balancedPageRanges(
        runs.size(), pagePayloadBytes, allocator,
        [&](std::size_t begin, std::size_t end) {
            return allocatorIndexUsedLength(runs, begin, end, retired);
        });
    StoredVector<Node, Allocator> nodes{NodeAllocator{allocator}};
    nodes.reserve(ranges.size());
    for (const auto [begin, end] : ranges) {
        auto payload = encodeAllocatorIndexLeaf<Limits>(
            runs, begin, end, retired, allocator);
        auto prepared = prepareExtent(payload, leafKind);
        StoredBytes<Allocator> minimum{ByteAllocator{allocator}};
        const auto encodedKey = allocatorRunKey(runs[begin], retired);
        minimum.assign(encodedKey.begin(), encodedKey.begin() + keyLength);
        nodes.push_back(Node{
            prepared.reference, std::move(minimum), 0});
        extents.push_back(std::move(prepared));
    }
    while (nodes.size() != 1) {
        ranges = balancedPageRanges(
            nodes.size(), pagePayloadBytes, allocator,
            [&](std::size_t begin, std::size_t end) {
                return internalUsedLength<Allocator>(nodes, begin, end);
            });
        StoredVector<Node, Allocator> parents{NodeAllocator{allocator}};
        parents.reserve(ranges.size());
        for (const auto [begin, end] : ranges) {
            auto payload = encodeInternalPage<Limits>(
                nodes, begin, end, allocator, role);
            auto prepared = prepareExtent(payload, internalKind);
            parents.push_back(Node{
                prepared.reference,
                nodes[begin].minimumKey,
                nodes[begin].level + 1});
            extents.push_back(std::move(prepared));
        }
        nodes = std::move(parents);
    }
    return nodes.front().reference;
}

template<class Allocator>
inline void putMutableTree(
    MutableTreeNode<Allocator>& node,
    ByteView key,
    ByteView value,
    const Allocator& allocator) {
    using ByteAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<std::byte>;
    node.dirty = true;
    if (node.level != 0) {
        auto& child = node.children[mutableChildFor(node, key)];
        putMutableTree(child, key, value, allocator);
        node.minimumKey = node.children.front().minimumKey;
        return;
    }
    const auto found = std::lower_bound(
        node.values.begin(), node.values.end(), key,
        [](const auto& entry, ByteView sought) {
            return UnsignedBytesLess{}(entry.key, sought);
        });
    StoredBytes<Allocator> storedKey{ByteAllocator{allocator}};
    StoredBytes<Allocator> storedValue{ByteAllocator{allocator}};
    storedKey.assign(key.begin(), key.end());
    storedValue.assign(value.begin(), value.end());
    if (found != node.values.end() && !UnsignedBytesLess{}(key, found->key)) {
        found->value = std::move(storedValue);
        found->overflow = {};
    } else {
        node.values.insert(
            found,
            MutableTreeValue<Allocator>{
                std::move(storedKey), std::move(storedValue), {}});
    }
    node.minimumKey = node.values.front().key;
}

template<class Allocator>
[[nodiscard]] inline bool eraseMutableTree(
    MutableTreeNode<Allocator>& node,
    ByteView key) {
    if (node.level == 0) {
        const auto found = std::lower_bound(
            node.values.begin(), node.values.end(), key,
            [](const auto& entry, ByteView sought) {
                return UnsignedBytesLess{}(entry.key, sought);
            });
        if (found == node.values.end() || UnsignedBytesLess{}(key, found->key)) {
            return false;
        }
        node.values.erase(found);
        node.dirty = true;
        if (!node.values.empty()) {
            node.minimumKey = node.values.front().key;
        }
        return true;
    }
    const auto childIndex = mutableChildFor(node, key);
    if (!eraseMutableTree(node.children[childIndex], key)) {
        return false;
    }
    node.dirty = true;
    if (node.children[childIndex].level == 0 &&
        node.children[childIndex].values.empty()) {
        node.children.erase(node.children.begin() + childIndex);
    } else if (node.children[childIndex].level != 0 &&
               node.children[childIndex].children.empty()) {
        node.children.erase(node.children.begin() + childIndex);
    }
    if (!node.children.empty()) {
        node.minimumKey = node.children.front().minimumKey;
    }
    return true;
}

template<class Limits, class Allocator>
[[nodiscard]] inline MutableTreeNode<Allocator> buildMutableTree(
    const OrderedKeyValues<Allocator>& values,
    const Allocator& allocator) {
    MutableTreeNode<Allocator> empty{allocator};
    if (values.empty()) {
        return empty;
    }
    using Entry = PersistedLeafEntry<Allocator>;
    using EntryAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<Entry>;
    using Node = MutableTreeNode<Allocator>;
    using NodeAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<Node>;
    constexpr auto pagePayloadBytes = std::max<std::uint64_t>(
        16U * 1024U, Limits::allocationQuantumBytes) -
        ExtentLayout::bytes - authenticationTagBytes;
    StoredVector<Entry, Allocator> entries{EntryAllocator{allocator}};
    entries.reserve(values.size());
    for (const auto& [key, value] : values) {
        const auto overflow = value.size() > Limits::maxInlineValueBytes
            ? ExtentReference{1, 1, 1, 1}
            : ExtentReference{};
        entries.push_back(Entry{&key, &value, overflow});
    }
    auto ranges = balancedPageRanges(
        entries.size(), pagePayloadBytes, allocator,
        [&](std::size_t begin, std::size_t end) {
            return leafUsedLength<Limits, Allocator>(entries, begin, end);
        });
    StoredVector<Node, Allocator> nodes{NodeAllocator{allocator}};
    nodes.reserve(ranges.size());
    for (const auto [begin, end] : ranges) {
        nodes.emplace_back(allocator);
        auto& leaf = nodes.back();
        leaf.minimumKey = *entries[begin].key;
        leaf.values.reserve(end - begin);
        for (auto index = begin; index != end; ++index) {
            leaf.values.push_back(MutableTreeValue<Allocator>{
                *entries[index].key,
                *entries[index].value,
                {}});
        }
    }
    while (nodes.size() != 1) {
        ranges = balancedPageRanges(
            nodes.size(), pagePayloadBytes, allocator,
            [&](std::size_t begin, std::size_t end) {
                return internalUsedLength<Allocator>(nodes, begin, end);
            });
        StoredVector<Node, Allocator> parents{NodeAllocator{allocator}};
        parents.reserve(ranges.size());
        for (const auto [begin, end] : ranges) {
            parents.emplace_back(allocator);
            auto& parent = parents.back();
            parent.minimumKey = nodes[begin].minimumKey;
            parent.level = nodes[begin].level + 1;
            parent.children.reserve(end - begin);
            for (auto index = begin; index != end; ++index) {
                parent.children.push_back(std::move(nodes[index]));
            }
        }
        nodes = std::move(parents);
    }
    return std::move(nodes.front());
}

struct PreparedPublication {
    PublicationSlot slot;
    PublicationPlaintext plaintext;
    std::uint16_t slotIndex;
};

[[nodiscard]] inline PublicationSlot encodePublicationSlot(
    OpenedDatabase& opened,
    const PublicationPlaintext& plaintext,
    std::uint16_t slotIndex,
    ProviderSet& providers) {
    PublicationSlot slot{};
    MutableByteView output{slot};
    writeBytes(output, SlotEnvelopeLayout::magic, "MIARESLT");
    writeLittleEndian<std::uint16_t>(1, output, SlotEnvelopeLayout::version);
    writeLittleEndian<std::uint16_t>(slotIndex, output, SlotEnvelopeLayout::index);
    writeLittleEndian<std::uint32_t>(
        publicationEnvelopeBytes, output, SlotEnvelopeLayout::length);
    writeLittleEndian<std::uint32_t>(
        publicationPlaintextBytes, output, SlotEnvelopeLayout::ciphertextLength);
    std::array<std::byte, aeadNonceBytes> nonce{};
    auto& crypto = ProviderAccess::crypto(providers);
    crypto.randomBytes(nonce);
    writeBytes(output, SlotEnvelopeLayout::nonce, nonce);
    const auto associatedData = publicationAssociatedData(
        opened.bootstrap, ByteView{slot}.first(publicationEnvelopeBytes));
    crypto.encryptDetached(
        opened.keys.header.view(),
        nonce,
        plaintext,
        associatedData,
        MutableByteView{slot}.subspan(
            SlotEnvelopeLayout::ciphertext, publicationPlaintextBytes),
        MutableByteView{slot}.subspan(
            SlotEnvelopeLayout::tag, authenticationTagBytes));
    return slot;
}

template<class Limits>
[[nodiscard]] inline PreparedPublication prepareExactPublication(
    OpenedDatabase& opened,
    const ExtentReference& orderedRoot,
    const ExtentReference& blobRoot,
    const ExtentReference& allocatorRoot,
    std::uint64_t highWaterBytes,
    ProviderSet& providers) {
    const auto generation = opened.format.generation + 1;
    const auto slotIndex = static_cast<std::uint16_t>(generation % 2);
    auto plaintext = opened.publication;
    MutableByteView plaintextOutput{plaintext};
    writeLittleEndian<std::uint16_t>(
        slotIndex, plaintextOutput, PublicationLayout::slotIndex);
    writeLittleEndian<std::uint64_t>(
        generation, plaintextOutput, PublicationLayout::generation);
    writeLittleEndian<std::uint64_t>(
        opened.format.generation,
        plaintextOutput,
        PublicationLayout::predecessorGeneration);
    const auto rootBytes = encodeExtentReference(orderedRoot);
    writeBytes(plaintextOutput, PublicationLayout::orderedRoot, rootBytes);
    writeBytes(
        plaintextOutput,
        PublicationLayout::blobRoot,
        encodeExtentReference(blobRoot));
    writeBytes(
        plaintextOutput,
        PublicationLayout::allocatorRoot,
        encodeExtentReference(allocatorRoot));
    writeLittleEndian<std::uint64_t>(
        highWaterBytes / Limits::allocationQuantumBytes,
        plaintextOutput,
        PublicationLayout::highWaterBlocks);

    auto slot = encodePublicationSlot(opened, plaintext, slotIndex, providers);
    return PreparedPublication{std::move(slot), std::move(plaintext), slotIndex};
}

template<class Limits, class Allocator>
inline void commitExact(
    DatabaseSession<Allocator, Limits>& session,
    const OrderedKeyValues<Allocator>& values,
    const BlobCatalog<Allocator>& blobs,
    const BlobWriteState<Allocator, Limits>& blobState) {
    const auto generation = session.opened.format.generation + 1;
    if (generation == 0) {
        throw DatabaseError{Errc::ResourceLimit, "database generation is exhausted"};
    }
    const auto startBlock =
        session.opened.format.highWaterBytes / Limits::allocationQuantumBytes;
    auto committedValues = makeOrderedKeyValues(session.allocator);
    committedValues = values;
    using CursorTreeAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<MutableTreeNode<Allocator>>;
    auto committedCursorTree = std::allocate_shared<MutableTreeNode<Allocator>>(
        CursorTreeAllocator{session.allocator},
        buildMutableTree<Limits>(values, session.allocator));
    using LeafEntry = PersistedLeafEntry<Allocator>;
    using LeafEntryAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<LeafEntry>;
    using ExtentAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<PreparedExactExtent<Allocator>>;
    using NodeAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<PreparedTreeNode<Allocator>>;
    using NodeVector = std::vector<PreparedTreeNode<Allocator>, NodeAllocator>;
    std::vector<PreparedExactExtent<Allocator>, ExtentAllocator> extents{
        ExtentAllocator{session.allocator}};
    NodeVector nodes{NodeAllocator{session.allocator}};
    ExtentReferences<Allocator> retainedReferences{
        typename std::allocator_traits<Allocator>::
            template rebind_alloc<ExtentReference>{session.allocator}};
    ExtentReference root;
    ExtentReference blobRoot;
    ExtentReference allocatorRoot;
    auto nextBlock = startBlock;
    using RunAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<ExtentRun>;
    std::vector<ExtentRun, RunAllocator> freeRuns{
        RunAllocator{session.allocator}};
    ExtentRuns<Allocator> retiredRuns{
        typename std::allocator_traits<Allocator>::
            template rebind_alloc<ExtentRun>{session.allocator}};
    ExtentReferences<Allocator> reachable{
        typename std::allocator_traits<Allocator>::
            template rebind_alloc<ExtentReference>{session.allocator}};
    MutableTreeNode<Allocator> mutableRoot{session.allocator};
    (void)loadExactValues<Limits>(
        *session.file,
        session.opened,
        *session.providers,
        session.allocator,
        &reachable,
        &mutableRoot);
    (void)loadBlobCatalog<Limits>(
        *session.file,
        session.opened,
        *session.providers,
        session.allocator,
        &reachable);
    ExtentRuns<Allocator> persistedFreeRuns{RunAllocator{session.allocator}};
    ExtentRuns<Allocator> persistedRetiredRuns{RunAllocator{session.allocator}};
    loadAllocatorReferences<Limits>(
        *session.file,
        session.opened,
        *session.providers,
        session.allocator,
        reachable,
        &persistedFreeRuns,
        &persistedRetiredRuns);
    freeRuns.assign(persistedFreeRuns.begin(), persistedFreeRuns.end());
    std::uint64_t oldestReaderGeneration =
        std::numeric_limits<std::uint64_t>::max();
    {
        std::lock_guard lock{session.mutex};
        for (const auto& [identity, reader] : session.activeReaders) {
            (void)identity;
            oldestReaderGeneration = std::min(
                oldestReaderGeneration, reader.generation);
        }
    }
    for (const auto& run : persistedRetiredRuns) {
        if (oldestReaderGeneration < run.retirementGeneration) {
            retiredRuns.push_back(run);
        } else {
            freeRuns.push_back(ExtentRun{run.start, run.count});
        }
    }
    std::sort(
        freeRuns.begin(), freeRuns.end(),
        [](const auto& left, const auto& right) {
            return left.start < right.start;
        });
    ExtentRuns<Allocator> coalescedFreeRuns{RunAllocator{session.allocator}};
    for (const auto& run : freeRuns) {
        if (!coalescedFreeRuns.empty() &&
            coalescedFreeRuns.back().start + coalescedFreeRuns.back().count ==
                run.start) {
            coalescedFreeRuns.back().count += run.count;
        } else {
            coalescedFreeRuns.push_back(run);
        }
    }
    freeRuns = std::move(coalescedFreeRuns);
    if (blobState.allocatorInitialized) {
        nextBlock = std::max(nextBlock, blobState.stagingNextBlock);
        for (const auto& run : blobState.stagingFreeRuns) {
            const auto runEnd = run.start + run.count;
            if (runEnd > startBlock) {
                freeRuns.push_back(ExtentRun{
                    std::max(run.start, startBlock),
                    runEnd - std::max(run.start, startBlock)});
            }
        }
        for (const auto& reference : blobState.discardedStagedReferences) {
            if (reference.blockIndex >= startBlock) {
                freeRuns.push_back(ExtentRun{
                    reference.blockIndex, reference.blockCount});
            }
        }
        const auto removeStagedRange = [&](const ExtentReference& reference) {
            ExtentRuns<Allocator> remaining{RunAllocator{session.allocator}};
            const auto start = reference.blockIndex;
            const auto end = start + reference.blockCount;
            for (const auto& run : freeRuns) {
                const auto runEnd = run.start + run.count;
                if (end <= run.start || start >= runEnd) {
                    remaining.push_back(run);
                    continue;
                }
                if (start > run.start) {
                    remaining.push_back(ExtentRun{
                        run.start, start - run.start});
                }
                if (end < runEnd) {
                    remaining.push_back(ExtentRun{
                        end, runEnd - end});
                }
            }
            freeRuns = std::move(remaining);
        };
        for (const auto& [id, version] : blobs) {
            (void)id;
            if (version->pending) {
                for (const auto& reference : version->chunks) {
                    removeStagedRange(reference);
                }
            }
        }
        coalesceBlobStagingFreeRuns(freeRuns, session.allocator);
    }
    std::sort(
        reachable.begin(), reachable.end(),
        [](const auto& left, const auto& right) {
            return left.blockIndex < right.blockIndex;
        });
    for (const auto& reference : reachable) {
        if (!retiredRuns.empty() &&
            retiredRuns.back().retirementGeneration == generation &&
            retiredRuns.back().start + retiredRuns.back().count ==
                reference.blockIndex) {
            retiredRuns.back().count += reference.blockCount;
        } else {
            retiredRuns.push_back(ExtentRun{
                reference.blockIndex, reference.blockCount, generation});
        }
    }
    auto allocateBlocks = [&](std::uint64_t count) {
        for (auto& run : freeRuns) {
            if (run.count >= count) {
                const auto allocated = run.start;
                run.start += count;
                run.count -= count;
                return allocated;
            }
        }
        const auto allocated = nextBlock;
        nextBlock += count;
        return allocated;
    };
    auto prepareExtent = [&](ByteView decoded, std::uint16_t unitKind) {
        auto prepared = prepareAuthenticatedExtent<Limits>(
            decoded,
            unitKind,
            generation,
            nextBlock,
            session.opened,
            *session.providers,
            session.allocator);
        const auto allocated = allocateBlocks(prepared.reference.blockCount);
        if (allocated != prepared.reference.blockIndex) {
            prepared = prepareAuthenticatedExtent<Limits>(
                decoded,
                unitKind,
                generation,
                allocated,
                session.opened,
                *session.providers,
                session.allocator);
        }
        return prepared;
    };
    if (!values.empty()) {
        if (mutableRoot.reference.null()) {
            mutableRoot.level = 0;
            mutableRoot.dirty = true;
        }
        for (const auto& [key, oldValue] : session.values) {
            if (!values.contains(key)) {
                (void)eraseMutableTree(mutableRoot, key);
            }
        }
        if (mutableRoot.level != 0 && mutableRoot.children.empty()) {
            mutableRoot.reference = {};
            mutableRoot.minimumKey.clear();
            mutableRoot.level = 0;
            mutableRoot.dirty = true;
        }
        for (const auto& [key, value] : values) {
            const auto old = session.values.find(key);
            if (old == session.values.end() || old->second != value) {
                putMutableTree(mutableRoot, key, value, session.allocator);
            }
        }
        while (mutableRoot.level != 0 && mutableRoot.children.size() == 1) {
            auto collapsedRoot = std::move(mutableRoot.children.front());
            mutableRoot = std::move(collapsedRoot);
            mutableRoot.dirty = true;
        }

        constexpr auto pagePayloadBytes = std::max<std::uint64_t>(
            16U * 1024U, Limits::allocationQuantumBytes) -
            ExtentLayout::bytes - authenticationTagBytes;
        const auto retainSubtree = [&](const auto& self, const auto& node) -> void {
            retainedReferences.push_back(node.reference);
            if (node.level == 0) {
                for (const auto& item : node.values) {
                    if (!item.overflow.null()) {
                        retainedReferences.push_back(item.overflow);
                    }
                }
                return;
            }
            for (const auto& child : node.children) {
                self(self, child);
            }
        };
        const auto persistNode = [&](
            const auto& self,
            MutableTreeNode<Allocator>& node) -> NodeVector {
            if (!node.dirty) {
                retainSubtree(retainSubtree, node);
                NodeVector clean{NodeAllocator{session.allocator}};
                clean.push_back(PreparedTreeNode<Allocator>{
                    node.reference, node.minimumKey, node.level});
                return clean;
            }
            if (node.level == 0) {
                StoredVector<LeafEntry, Allocator> leafEntries{
                    LeafEntryAllocator{session.allocator}};
                leafEntries.reserve(node.values.size());
                for (auto& item : node.values) {
                    if (item.value.size() > Limits::maxInlineValueBytes &&
                        item.overflow.null()) {
                        auto overflow = prepareExtent(item.value, 11);
                        item.overflow = overflow.reference;
                        extents.push_back(std::move(overflow));
                    } else if (!item.overflow.null()) {
                        retainedReferences.push_back(item.overflow);
                    }
                    leafEntries.push_back(LeafEntry{
                        &item.key, &item.value, item.overflow});
                }
                const auto ranges = balancedPageRanges(
                    leafEntries.size(), pagePayloadBytes, session.allocator,
                    [&](std::size_t begin, std::size_t end) {
                        return leafUsedLength<Limits, Allocator>(
                            leafEntries, begin, end);
                    });
                NodeVector result{NodeAllocator{session.allocator}};
                for (const auto [begin, end] : ranges) {
                    auto payload = encodeLeafPage<Limits>(
                        leafEntries, begin, end, session.allocator);
                    auto prepared = prepareExtent(payload, 2);
                    result.push_back(PreparedTreeNode<Allocator>{
                        prepared.reference, *leafEntries[begin].key, 0});
                    extents.push_back(std::move(prepared));
                }
                return result;
            }
            NodeVector persistedChildren{NodeAllocator{session.allocator}};
            for (auto& child : node.children) {
                auto replacements = self(self, child);
                persistedChildren.insert(
                    persistedChildren.end(),
                    std::make_move_iterator(replacements.begin()),
                    std::make_move_iterator(replacements.end()));
            }
            const auto ranges = balancedPageRanges(
                persistedChildren.size(), pagePayloadBytes, session.allocator,
                [&](std::size_t begin, std::size_t end) {
                    return internalUsedLength<Allocator>(
                        persistedChildren, begin, end);
                });
            NodeVector result{NodeAllocator{session.allocator}};
            for (const auto [begin, end] : ranges) {
                auto payload = encodeInternalPage<Limits>(
                    persistedChildren, begin, end, session.allocator);
                auto prepared = prepareExtent(payload, 1);
                result.push_back(PreparedTreeNode<Allocator>{
                    prepared.reference,
                    persistedChildren[begin].minimumKey,
                    persistedChildren[begin].level + 1});
                extents.push_back(std::move(prepared));
            }
            return result;
        };
        nodes = persistNode(persistNode, mutableRoot);
        while (nodes.size() != 1) {
            const auto ranges = balancedPageRanges(
                nodes.size(), pagePayloadBytes, session.allocator,
                [&](std::size_t begin, std::size_t end) {
                    return internalUsedLength<Allocator>(nodes, begin, end);
                });
            NodeVector parents{NodeAllocator{session.allocator}};
            for (const auto [begin, end] : ranges) {
                auto payload = encodeInternalPage<Limits>(
                    nodes, begin, end, session.allocator);
                auto prepared = prepareExtent(payload, 1);
                parents.push_back(PreparedTreeNode<Allocator>{
                    prepared.reference,
                    nodes[begin].minimumKey,
                    nodes[begin].level + 1});
                extents.push_back(std::move(prepared));
            }
            nodes = std::move(parents);
        }
        root = nodes.front().reference;
    }
    using FixedEntry = FixedLeafEntry<Allocator>;
    using FixedEntryAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<FixedEntry>;
    using ByteAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<std::byte>;
    using ReferenceAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<ExtentReference>;
    using VersionAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<BlobVersion<Allocator>>;
    auto committedBlobs = makeBlobCatalog(session.allocator);
    StoredVector<FixedEntry, Allocator> catalogEntries{
        FixedEntryAllocator{session.allocator}};
    catalogEntries.reserve(blobs.size());
    const auto prepareOwnedExtent = [&]<class Owner>(
        ByteView decoded,
        std::uint16_t unitKind,
        bool compressionEligible,
        std::uint64_t sequence,
        const Owner& owner) {
        auto prepared = prepareAuthenticatedExtent<Limits>(
            decoded,
            unitKind,
            generation,
            nextBlock,
            session.opened,
            *session.providers,
            session.allocator,
            compressionEligible,
            4,
            sequence,
            owner);
        const auto allocated = allocateBlocks(prepared.reference.blockCount);
        if (allocated != prepared.reference.blockIndex) {
            prepared = prepareAuthenticatedExtent<Limits>(
                decoded,
                unitKind,
                generation,
                allocated,
                session.opened,
                *session.providers,
                session.allocator,
                compressionEligible,
                4,
                sequence,
                owner);
        }
        return prepared;
    };
    for (const auto& [id, version] : blobs) {
        const auto idBytes = id.toBytes();
        StoredBytes<Allocator> catalogKey{ByteAllocator{session.allocator}};
        catalogKey.assign(idBytes.begin(), idBytes.end());
        if (!version->pending) {
            retainedReferences.insert(
                retainedReferences.end(),
                version->reachable.begin(),
                version->reachable.end());
            catalogEntries.push_back(FixedEntry{
                std::move(catalogKey), version->manifest});
            committedBlobs.emplace(id, version);
            continue;
        }

        ExtentReferences<Allocator> chunkReferences{
            ReferenceAllocator{session.allocator}};
        ExtentReferences<Allocator> versionReachable{
            ReferenceAllocator{session.allocator}};
        StoredVector<FixedEntry, Allocator> chunkEntries{
            FixedEntryAllocator{session.allocator}};
        const auto chunkCount = version->size == 0
            ? 0
            : 1 + (version->size - 1) / Limits::blobChunkBytes;
        if (version->chunks.size() != chunkCount) {
            throw ContractError{
                Errc::InvalidState,
                "finalized Blob has an incomplete staged chunk set"};
        }
        chunkReferences.reserve(static_cast<std::size_t>(chunkCount));
        chunkEntries.reserve(static_cast<std::size_t>(chunkCount));
        for (std::uint64_t ordinal = 0; ordinal != chunkCount; ++ordinal) {
            const auto reference = version->chunks[ordinal];
            chunkReferences.push_back(reference);
            versionReachable.push_back(reference);
            StoredBytes<Allocator> ordinalKey{
                ByteAllocator{session.allocator}};
            ordinalKey.resize(8);
            writeLittleEndian<std::uint64_t>(ordinal, ordinalKey, 0);
            chunkEntries.push_back(FixedEntry{
                std::move(ordinalKey), reference});
        }
        const auto chunkIndexBegin = extents.size();
        const auto prepareChunkIndex = [&](ByteView decoded, std::uint16_t kind) {
            return prepareOwnedExtent(decoded, kind, true, 0, idBytes);
        };
        const auto chunkRoot = persistFixedTree<Limits>(
            chunkEntries,
            3,
            5,
            6,
            session.allocator,
            prepareChunkIndex,
            extents);
        for (auto index = chunkIndexBegin; index != extents.size(); ++index) {
            versionReachable.push_back(extents[index].reference);
        }
        std::array<std::byte, BlobManifestLayout::bytes> manifest{};
        MutableByteView manifestOutput{manifest};
        writeBytes(manifestOutput, BlobManifestLayout::magic, "MIAREBLB");
        writeLittleEndian<std::uint16_t>(
            1, manifestOutput, BlobManifestLayout::version);
        writeLittleEndian<std::uint32_t>(
            BlobManifestLayout::bytes,
            manifestOutput,
            BlobManifestLayout::length);
        writeBytes(manifestOutput, BlobManifestLayout::id, idBytes);
        writeLittleEndian<std::uint64_t>(
            generation, manifestOutput, BlobManifestLayout::generation);
        writeLittleEndian<std::uint64_t>(
            version->size, manifestOutput, BlobManifestLayout::logicalLength);
        writeLittleEndian<std::uint64_t>(
            Limits::blobChunkBytes,
            manifestOutput,
            BlobManifestLayout::chunkSize);
        writeLittleEndian<std::uint64_t>(
            chunkCount, manifestOutput, BlobManifestLayout::chunkCount);
        writeBytes(
            manifestOutput,
            BlobManifestLayout::chunkRoot,
            encodeExtentReference(chunkRoot));
        auto preparedManifest = prepareOwnedExtent(
            manifest, 12, false, 0, idBytes);
        const auto manifestReference = preparedManifest.reference;
        versionReachable.insert(
            versionReachable.begin(), manifestReference);
        extents.push_back(std::move(preparedManifest));
        auto committedVersion = std::allocate_shared<BlobVersion<Allocator>>(
            VersionAllocator{session.allocator},
            id,
            version->size,
            generation,
            manifestReference,
            chunkRoot,
            std::move(chunkReferences),
            std::move(versionReachable));
        catalogEntries.push_back(FixedEntry{
            std::move(catalogKey), manifestReference});
        committedBlobs.emplace(id, std::move(committedVersion));
    }
    const auto prepareCatalog = [&](ByteView decoded, std::uint16_t kind) {
        return prepareExtent(decoded, kind);
    };
    blobRoot = persistFixedTree<Limits>(
        catalogEntries,
        2,
        3,
        4,
        session.allocator,
        prepareCatalog,
        extents);
    subtractRetainedReferences(
        retiredRuns, retainedReferences, session.allocator);
    const auto eraseEmptyRuns = [&] {
        freeRuns.erase(
            std::remove_if(
                freeRuns.begin(),
                freeRuns.end(),
                [](const auto& run) { return run.count == 0; }),
            freeRuns.end());
    };
    ExtentReference freeRoot;
    ExtentReference retiredRoot;
    const auto allocatorBlock = allocateBlocks(1);
    eraseEmptyRuns();
    constexpr auto metadataPageBlocks = std::max<std::uint64_t>(
        16U * 1024U, Limits::allocationQuantumBytes) /
        Limits::allocationQuantumBytes;
    ExtentReferences<Allocator> metadataReservations{
        typename std::allocator_traits<Allocator>::
            template rebind_alloc<ExtentReference>{session.allocator}};
    ExtentRuns<Allocator> representedFreeRuns{
        freeRuns.begin(), freeRuns.end(), RunAllocator{session.allocator}};
    ExtentRuns<Allocator> previousRepresentedFreeRuns{
        RunAllocator{session.allocator}};
    bool hasPreviousRepresentation = false;
    const auto coalesceFreeRuns = [&](ExtentRuns<Allocator> runs) {
        std::sort(
            runs.begin(), runs.end(),
            [](const auto& left, const auto& right) {
                return left.start < right.start;
            });
        ExtentRuns<Allocator> coalesced{RunAllocator{session.allocator}};
        for (const auto& run : runs) {
            if (run.count == 0) {
                continue;
            }
            if (!coalesced.empty() &&
                coalesced.back().start + coalesced.back().count == run.start) {
                coalesced.back().count += run.count;
            } else {
                coalesced.push_back(run);
            }
        }
        return coalesced;
    };
    const auto sameRuns = [](const auto& left, const auto& right) {
        return left.size() == right.size() && std::equal(
            left.begin(), left.end(), right.begin(),
            [](const auto& lhs, const auto& rhs) {
                return lhs.start == rhs.start && lhs.count == rhs.count &&
                    lhs.retirementGeneration == rhs.retirementGeneration;
            });
    };
    const auto removeFreeRange = [&](std::uint64_t start, std::uint64_t count) {
        ExtentRuns<Allocator> remaining{RunAllocator{session.allocator}};
        const auto end = start + count;
        for (const auto& run : freeRuns) {
            const auto runEnd = run.start + run.count;
            if (end <= run.start || start >= runEnd) {
                remaining.push_back(run);
                continue;
            }
            if (start > run.start) {
                remaining.push_back(ExtentRun{
                    run.start, start - run.start});
            }
            if (end < runEnd) {
                remaining.push_back(ExtentRun{end, runEnd - end});
            }
        }
        freeRuns = std::move(remaining);
    };
    constexpr std::size_t metadataConvergenceLimit = 128;
    bool metadataConverged = false;
    for (std::size_t attempt = 0;
         attempt != metadataConvergenceLimit && !metadataConverged;
         ++attempt) {
        StoredVector<PreparedExactExtent<Allocator>, Allocator> metadataExtents{
            ExtentAllocator{session.allocator}};
        std::size_t reservation = 0;
        const auto prepareReserved = [&](ByteView decoded, std::uint16_t kind) {
            const auto blockIndex = reservation < metadataReservations.size()
                ? metadataReservations[reservation].blockIndex
                : nextBlock;
            ++reservation;
            return prepareAuthenticatedExtent<Limits>(
                decoded,
                kind,
                generation,
                blockIndex,
                session.opened,
                *session.providers,
                session.allocator);
        };
        retiredRoot = persistAllocatorIndex<Limits>(
            retiredRuns,
            true,
            session.allocator,
            prepareReserved,
            metadataExtents);
        freeRoot = persistAllocatorIndex<Limits>(
            representedFreeRuns,
            false,
            session.allocator,
            prepareReserved,
            metadataExtents);
        if (reservation > metadataReservations.size()) {
            freeRuns.assign(
                representedFreeRuns.begin(), representedFreeRuns.end());
            while (metadataReservations.size() != reservation) {
                const auto blockIndex = allocateBlocks(metadataPageBlocks);
                metadataReservations.push_back(ExtentReference{
                    blockIndex, metadataPageBlocks, 0, generation});
            }
            eraseEmptyRuns();
            representedFreeRuns.assign(freeRuns.begin(), freeRuns.end());
            hasPreviousRepresentation = false;
            continue;
        }
        for (const auto& reserved : metadataReservations) {
            removeFreeRange(reserved.blockIndex, reserved.blockCount);
        }
        ExtentRuns<Allocator> finalFreeRuns{
            freeRuns.begin(), freeRuns.end(), RunAllocator{session.allocator}};
        for (std::size_t index = 0;
             index != metadataReservations.size();
             ++index) {
            const auto usedBlocks = index < reservation
                ? metadataExtents[index].reference.blockCount
                : 0;
            if (usedBlocks > metadataPageBlocks) {
                throw DatabaseError{
                    Errc::ResourceLimit,
                    "allocator metadata exceeds its full-page reservation"};
            }
            if (usedBlocks != metadataPageBlocks) {
                finalFreeRuns.push_back(ExtentRun{
                    metadataReservations[index].blockIndex + usedBlocks,
                    metadataPageBlocks - usedBlocks});
            }
        }
        finalFreeRuns = coalesceFreeRuns(std::move(finalFreeRuns));
        if (!sameRuns(finalFreeRuns, representedFreeRuns)) {
            if (hasPreviousRepresentation &&
                sameRuns(finalFreeRuns, previousRepresentedFreeRuns)) {
                freeRuns = finalFreeRuns;
                const auto blockIndex = allocateBlocks(metadataPageBlocks);
                metadataReservations.push_back(ExtentReference{
                    blockIndex, metadataPageBlocks, 0, generation});
                eraseEmptyRuns();
                representedFreeRuns.assign(freeRuns.begin(), freeRuns.end());
                hasPreviousRepresentation = false;
                continue;
            }
            previousRepresentedFreeRuns = representedFreeRuns;
            hasPreviousRepresentation = true;
            freeRuns = finalFreeRuns;
            representedFreeRuns = std::move(finalFreeRuns);
            continue;
        }
        freeRuns = std::move(finalFreeRuns);
        extents.insert(
            extents.end(),
            std::make_move_iterator(metadataExtents.begin()),
            std::make_move_iterator(metadataExtents.end()));
        metadataConverged = true;
    }
    if (!metadataConverged) {
        throw DatabaseError{
            Errc::ResourceLimit,
            "allocator metadata reservation did not converge"};
    }
    eraseEmptyRuns();
    std::uint64_t freeBlocks = 0;
    for (const auto& run : freeRuns) {
        freeBlocks += run.count;
    }
    std::uint64_t retiredBlocks = 0;
    decltype(session.retiredBlocksByGeneration) committedRetirementCounts{
        session.retiredBlocksByGeneration.key_comp(),
        session.retiredBlocksByGeneration.get_allocator()};
    for (const auto& run : retiredRuns) {
        retiredBlocks += run.count;
        committedRetirementCounts[run.retirementGeneration] += run.count;
    }
    std::array<std::byte, AllocatorRootLayout::bytes> allocatorPayload{};
    MutableByteView allocatorOutput{allocatorPayload};
    writeBytes(allocatorOutput, AllocatorRootLayout::magic, "MIAREALC");
    writeLittleEndian<std::uint16_t>(
        1, allocatorOutput, AllocatorRootLayout::version);
    writeLittleEndian<std::uint32_t>(
        AllocatorRootLayout::bytes, allocatorOutput, AllocatorRootLayout::length);
    writeLittleEndian<std::uint64_t>(
        generation, allocatorOutput, AllocatorRootLayout::generation);
    writeLittleEndian<std::uint64_t>(
        nextBlock, allocatorOutput, AllocatorRootLayout::highWaterBlocks);
    writeBytes(
        allocatorOutput,
        AllocatorRootLayout::freeRoot,
        encodeExtentReference(freeRoot));
    writeBytes(
        allocatorOutput,
        AllocatorRootLayout::retiredRoot,
        encodeExtentReference(retiredRoot));
    std::uint64_t reachableBlocks = 1;
    for (const auto& extent : extents) {
        reachableBlocks += extent.reference.blockCount;
    }
    for (const auto& [id, version] : blobs) {
        (void)id;
        if (version->pending) {
            for (const auto& reference : version->chunks) {
                reachableBlocks += reference.blockCount;
            }
        }
    }
    for (const auto& retained : retainedReferences) {
        reachableBlocks += retained.blockCount;
    }
    writeLittleEndian<std::uint64_t>(
        reachableBlocks, allocatorOutput, AllocatorRootLayout::reachableBlocks);
    writeLittleEndian<std::uint64_t>(
        freeBlocks, allocatorOutput, AllocatorRootLayout::freeBlocks);
    writeLittleEndian<std::uint64_t>(
        retiredBlocks, allocatorOutput, AllocatorRootLayout::retiredBlocks);
    auto preparedAllocator = prepareAuthenticatedExtent<Limits>(
        allocatorPayload,
        14,
        generation,
        allocatorBlock,
        session.opened,
        *session.providers,
        session.allocator,
        false);
    allocatorRoot = preparedAllocator.reference;
    extents.push_back(std::move(preparedAllocator));
    const auto highWaterBytes = nextBlock * Limits::allocationQuantumBytes;
    const auto growthBytes = highWaterBytes - session.opened.format.highWaterBytes;
    if (growthBytes > Limits::maxFileGrowthPerTransaction ||
        highWaterBytes > Limits::maxDatabaseBytes) {
        throw DatabaseError{
            Errc::ResourceLimit,
            "commit would exceed the capacity profile"};
    }
    auto publication = prepareExactPublication<Limits>(
        session.opened,
        root,
        blobRoot,
        allocatorRoot,
        highWaterBytes,
        *session.providers);

    bool publicationStarted = false;
    const auto recordAbandonedTail = [&]() noexcept {
        try {
            const auto physicalBytes = session.file->size();
            std::lock_guard lock{session.mutex};
            session.opened.abandonedTailBytes =
                physicalBytes > session.opened.format.highWaterBytes
                ? physicalBytes - session.opened.format.highWaterBytes
                : 0;
        } catch (...) {
        }
    };
    try {
        session.file->resize(highWaterBytes);
        for (const auto& extent : extents) {
            session.file->writeExactAt(
                extent.reference.blockIndex * Limits::allocationQuantumBytes,
                extent.bytes);
        }
        session.file->stableStorageBarrier();
        publicationStarted = true;
        session.file->writeExactAt(
            bootstrapBytes + publication.slotIndex * publicationSlotBytes,
            publication.slot);
        session.file->stableStorageBarrier();
    } catch (const DatabaseError& error) {
        recordAbandonedTail();
        session.recoveryCause.store(
            publicationStarted
                ? RecoveryCause::CommitOutcomeUnknown
                : RecoveryCause::CommitKnownUnpublished,
            std::memory_order_release);
        session.state.store(
            DatabaseState::RecoveryRequired, std::memory_order_release);
        throw DatabaseError{
            publicationStarted ? Errc::CommitOutcomeUnknown : Errc::CommitFailed,
            publicationStarted
                ? "commit publication outcome is unknown"
                : "commit failed before publication",
            error.nativeCode()};
    } catch (...) {
        recordAbandonedTail();
        session.recoveryCause.store(
            RecoveryCause::CommitOutcomeUnknown,
            std::memory_order_release);
        session.state.store(
            DatabaseState::RecoveryRequired, std::memory_order_release);
        throw;
    }

    {
        std::lock_guard lock{session.mutex};
        session.opened.publication = std::move(publication.plaintext);
        session.opened.format.generation = generation;
        session.opened.format.highWaterBytes = highWaterBytes;
        session.opened.format.orderedRoot = encodeExtentReference(root);
        session.opened.format.blobRoot = encodeExtentReference(blobRoot);
        session.opened.format.allocatorRoot = encodeExtentReference(allocatorRoot);
        session.opened.rejectedInactivePublication = false;
        session.opened.abandonedTailBytes = 0;
        session.values = std::move(committedValues);
        session.blobs = std::move(committedBlobs);
        session.cursorTree = std::move(committedCursorTree);
        session.valuesLoaded = true;
        session.blobsLoaded = true;
        session.liveBlocks =
            commonRegionBytes / Limits::allocationQuantumBytes + reachableBlocks;
        session.retiredBlocksByGeneration = std::move(
            committedRetirementCounts);
        session.allocatorSnapshotLoaded = true;
    }
}

} // namespace miare::detail
