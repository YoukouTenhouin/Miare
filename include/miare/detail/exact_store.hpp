#pragma once

#include <miare/detail/database_format.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace miare::detail {

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

template<class Allocator, class Limits>
struct DatabaseSession {
    DatabaseSession(
        std::unique_ptr<DurableFile> openedFile,
        ProviderSet openedProviders,
        Allocator openedAllocator,
        OpenedDatabase openedDatabase,
        OrderedKeyValues<Allocator> openedValues,
        std::uint32_t configuredMaxReaders)
        : file(std::move(openedFile)),
          providers(std::move(openedProviders)),
          allocator(std::move(openedAllocator)),
          opened(std::move(openedDatabase)),
          values(std::move(openedValues)),
          maxReaders(configuredMaxReaders) {}

    std::unique_ptr<DurableFile> file;
    std::optional<ProviderSet> providers;
    Allocator allocator;
    OpenedDatabase opened;
    OrderedKeyValues<Allocator> values;
    std::mutex mutex;
    std::condition_variable writerAvailable;
    std::uint64_t nextWriterTicket = 0;
    std::uint64_t servingWriterTicket = 0;
    std::size_t waitingWriters = 0;
    std::size_t activeReaders = 0;
    bool writerActive = false;
    std::size_t liveTransactions = 0;
    std::uint32_t maxReaders;
    std::atomic<DatabaseState> state{DatabaseState::Open};
};

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
    const Allocator& allocator) {
    using ByteAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<std::byte>;
    auto& crypto = ProviderAccess::crypto(providers);
    StoredBytes<Allocator> stored{ByteAllocator{allocator}};
    stored.assign(decoded.begin(), decoded.end());
    std::uint32_t flags = 0;
    std::uint32_t codec = 0;
    if (opened.format.compression == Compression::ZStd && !decoded.empty()) {
        auto& compression = ProviderAccess::compression(providers);
        StoredBytes<Allocator> candidate{ByteAllocator{allocator}};
        candidate.resize(compression.compressBound(decoded.size()));
        const auto compressedBytes = compression.compress(decoded, candidate);
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
    writeLittleEndian<std::uint32_t>(2, output, ExtentLayout::keyDomain);
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
    std::array<std::byte, aeadNonceBytes> nonce{};
    crypto.randomBytes(nonce);
    writeBytes(output, ExtentLayout::nonce, nonce);
    const auto associatedData = extentAssociatedData(
        opened, ByteView{extent}.first(ExtentLayout::bytes));
    crypto.encryptDetached(
        opened.keys.mainData.view(),
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

template<class Allocator, class Nodes>
[[nodiscard]] inline std::uint64_t internalUsedLength(
    const Nodes& nodes,
    std::size_t begin,
    std::size_t end) {
    const auto prefixLength = end - begin == 1
        ? 0
        : commonPrefixLength(nodes[begin + 1].minimumKey, nodes[end - 1].minimumKey);
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
    const Allocator& allocator) {
    using ByteAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<std::byte>;
    constexpr auto payloadBytes = std::max<std::uint64_t>(
        16U * 1024U, Limits::allocationQuantumBytes) -
        ExtentLayout::bytes - authenticationTagBytes;
    StoredBytes<Allocator> payload{ByteAllocator{allocator}};
    payload.resize(payloadBytes);
    const auto count = end - begin - 1;
    const auto prefixLength = count == 0
        ? 0
        : commonPrefixLength(nodes[begin + 1].minimumKey, nodes[end - 1].minimumKey);
    const auto used = internalUsedLength<Allocator>(nodes, begin, end);
    if (used > payload.size()) {
        throw DatabaseError{Errc::ResourceLimit, "one separator cannot fit in an internal page"};
    }
    MutableByteView output{payload};
    writeBytes(output, PageLayout::magic, "MIAREPG\0");
    writeLittleEndian<std::uint16_t>(1, output, PageLayout::version);
    writeLittleEndian<std::uint16_t>(2, output, PageLayout::type);
    writeLittleEndian<std::uint32_t>(PageLayout::bytes, output, PageLayout::headerLength);
    writeLittleEndian<std::uint32_t>(1, output, PageLayout::role);
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
    const Allocator& allocator) {
    validateExtentReference<Limits>(reference, opened.format.generation, opened.format.highWaterBytes);
    constexpr auto pageCeiling = std::max<std::uint64_t>(
        16U * 1024U, Limits::allocationQuantumBytes);
    constexpr auto uncompressedBlockCount =
        pageCeiling / Limits::allocationQuantumBytes;
    const auto minimalBlockCount =
        reference.encodedLength / Limits::allocationQuantumBytes +
        (reference.encodedLength % Limits::allocationQuantumBytes != 0);
    const bool page = expectedKind == 1 || expectedKind == 2;
    if ((page && reference.blockCount > uncompressedBlockCount) ||
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
        readLittleEndian<std::uint32_t>(input, ExtentLayout::keyDomain) != 2 ||
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
        readLittleEndian<std::uint64_t>(input, ExtentLayout::sequence) != 0 ||
        !allZero(input, ExtentLayout::owner, ExtentLayout::nonce) ||
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
    constexpr auto framingBytes =
        ExtentLayout::bytes + authenticationTagBytes;
    if (decodedLength > std::numeric_limits<std::uint64_t>::max() -
            framingBytes - (Limits::allocationQuantumBytes - 1)) {
        throwCorrupt("authenticated extent decoded length overflows");
    }
    const auto uncompressedBlocks =
        (framingBytes + decodedLength +
         Limits::allocationQuantumBytes - 1) /
        Limits::allocationQuantumBytes;
    if (flags > 1 || codec != flags || codecProfile != flags ||
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
            opened.keys.mainData.view(),
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

template<class Limits, class Allocator>
inline OrderedPageBounds<Allocator> loadOrderedPage(
    DurableFile& file,
    const ExtentReference& reference,
    std::uint32_t expectedLevel,
    OpenedDatabase& opened,
    ProviderSet& providers,
    const Allocator& allocator,
    OrderedKeyValues<Allocator>& values,
    std::vector<ExtentReference>* reachable = nullptr) {
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
    std::vector<std::pair<StoredBytes<Allocator>, ExtentReference>> children;
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
            if (overflow) {
                const auto overflowReference = decodeExtentReference(
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
        return OrderedPageBounds<Allocator>{
            std::move(firstKey), std::move(previousKey)};
    }
    StoredBytes<Allocator> subtreeMinimum{ByteAllocator{allocator}};
    StoredBytes<Allocator> subtreeMaximum{ByteAllocator{allocator}};
    for (std::size_t index = 0; index != children.size(); ++index) {
        auto childBounds = loadOrderedPage<Limits>(
            file, children[index].second, level - 1,
            opened, providers, allocator, values, reachable);
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
    return OrderedPageBounds<Allocator>{
        std::move(subtreeMinimum), std::move(subtreeMaximum)};
}

template<class Limits, class Allocator>
[[nodiscard]] inline OrderedKeyValues<Allocator> loadExactValues(
    DurableFile& file,
    OpenedDatabase& opened,
    ProviderSet& providers,
    const Allocator& allocator,
    std::vector<ExtentReference>* reachable = nullptr) {
    auto values = makeOrderedKeyValues(allocator);
    const auto reference = decodeExtentReference(opened.format.orderedRoot);
    if (reference.null()) {
        return values;
    }
    validateOrderedPageReference<Limits>(
        reference, opened.format.generation, opened.format.highWaterBytes);
    std::array<std::byte, ExtentLayout::bytes> preamble{};
    file.readExactAt(reference.blockIndex * Limits::allocationQuantumBytes, preamble);
    const auto kind = readLittleEndian<std::uint16_t>(preamble, ExtentLayout::unitKind);
    if (kind != 1 && kind != 2) {
        throwCorrupt("ordered root role is invalid");
    }
    auto rootPayload = readAuthenticatedExtent<Limits>(
        file, reference, kind, std::nullopt, opened, providers, allocator);
    const auto level = readLittleEndian<std::uint32_t>(rootPayload, PageLayout::level);
    (void)loadOrderedPage<Limits>(
        file, reference, level, opened, providers, allocator, values, reachable);
    return values;
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
    const OrderedKeyValues<Allocator>& values) {
    const auto generation = session.opened.format.generation + 1;
    if (generation == 0) {
        throw DatabaseError{Errc::ResourceLimit, "database generation is exhausted"};
    }
    const auto startBlock =
        session.opened.format.highWaterBytes / Limits::allocationQuantumBytes;
    auto committedValues = makeOrderedKeyValues(session.allocator);
    committedValues = values;
    using LeafEntry = PersistedLeafEntry<Allocator>;
    using LeafEntryAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<LeafEntry>;
    using ExtentAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<PreparedExactExtent<Allocator>>;
    using NodeAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<PreparedTreeNode<Allocator>>;
    std::vector<LeafEntry, LeafEntryAllocator> entries{
        LeafEntryAllocator{session.allocator}};
    std::vector<PreparedExactExtent<Allocator>, ExtentAllocator> extents{
        ExtentAllocator{session.allocator}};
    std::vector<PreparedTreeNode<Allocator>, NodeAllocator> nodes{
        NodeAllocator{session.allocator}};
    ExtentReference root;
    auto nextBlock = startBlock;
    struct FreeRun {
        std::uint64_t start;
        std::uint64_t count;
    };
    using FreeRunAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<FreeRun>;
    std::vector<FreeRun, FreeRunAllocator> freeRuns{
        FreeRunAllocator{session.allocator}};
    const auto commonBlocks = commonRegionBytes / Limits::allocationQuantumBytes;
    if (startBlock > commonBlocks) {
        using ByteAllocator = typename std::allocator_traits<Allocator>::
            template rebind_alloc<std::byte>;
        StoredBytes<Allocator> occupied{ByteAllocator{session.allocator}};
        occupied.resize(startBlock - commonBlocks);
        std::vector<ExtentReference> reachable;
        (void)loadExactValues<Limits>(
            *session.file,
            session.opened,
            *session.providers,
            session.allocator,
            &reachable);
        for (const auto& reference : reachable) {
            const auto block = reference.blockIndex;
            const auto count = reference.blockCount;
            std::fill_n(
                occupied.begin() + static_cast<std::ptrdiff_t>(block - commonBlocks),
                static_cast<std::size_t>(count),
                std::byte{1});
        }
        std::uint64_t block = commonBlocks;
        while (block != startBlock) {
            if (occupied[block - commonBlocks] != std::byte{0}) {
                ++block;
                continue;
            }
            const auto runStart = block;
            while (block != startBlock &&
                   occupied[block - commonBlocks] == std::byte{0}) {
                ++block;
            }
            freeRuns.push_back(FreeRun{runStart, block - runStart});
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
        entries.reserve(values.size());
        for (const auto& [key, value] : values) {
            ExtentReference overflow;
            if (value.size() > Limits::maxInlineValueBytes) {
                auto prepared = prepareExtent(value, 11);
                overflow = prepared.reference;
                extents.push_back(std::move(prepared));
            }
            entries.push_back(LeafEntry{&key, &value, overflow});
        }

        constexpr auto pagePayloadBytes = std::max<std::uint64_t>(
            16U * 1024U, Limits::allocationQuantumBytes) -
            ExtentLayout::bytes - authenticationTagBytes;
        using Range = std::pair<std::size_t, std::size_t>;
        using RangeAllocator = typename std::allocator_traits<Allocator>::
            template rebind_alloc<Range>;
        std::vector<Range, RangeAllocator> ranges{
            RangeAllocator{session.allocator}};
        std::size_t begin = 0;
        auto scan = begin + 1;
        while (scan != entries.size()) {
            if (leafUsedLength<Limits, Allocator>(
                    entries, begin, scan + 1) <= pagePayloadBytes) {
                ++scan;
                continue;
            }
            std::size_t selected = 0;
            std::uint64_t selectedDifference =
                std::numeric_limits<std::uint64_t>::max();
            for (auto boundary = begin + 1; boundary != scan + 1; ++boundary) {
                const auto left = leafUsedLength<Limits, Allocator>(
                    entries, begin, boundary);
                const auto right = leafUsedLength<Limits, Allocator>(
                    entries, boundary, scan + 1);
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
                    "ordered leaf overflow has no valid split boundary"};
            }
            ranges.emplace_back(begin, selected);
            begin = selected;
            ++scan;
        }
        ranges.emplace_back(begin, entries.size());
        for (const auto [rangeBegin, rangeEnd] : ranges) {
            auto payload = encodeLeafPage<Limits>(
                entries, rangeBegin, rangeEnd, session.allocator);
            auto prepared = prepareExtent(payload, 2);
            auto minimum = StoredBytes<Allocator>{
                typename std::allocator_traits<Allocator>::
                    template rebind_alloc<std::byte>{session.allocator}};
            minimum = *entries[rangeBegin].key;
            nodes.push_back(PreparedTreeNode<Allocator>{
                prepared.reference, std::move(minimum), 0});
            extents.push_back(std::move(prepared));
        }

        while (nodes.size() != 1) {
            std::vector<PreparedTreeNode<Allocator>, NodeAllocator> parents{
                NodeAllocator{session.allocator}};
            ranges.clear();
            begin = 0;
            scan = begin + 1;
            while (scan != nodes.size()) {
                if (internalUsedLength<Allocator>(
                        nodes, begin, scan + 1) <= pagePayloadBytes) {
                    ++scan;
                    continue;
                }
                std::size_t selected = 0;
                std::uint64_t selectedDifference =
                    std::numeric_limits<std::uint64_t>::max();
                for (auto boundary = begin + 1;
                     boundary != scan + 1;
                     ++boundary) {
                    const auto left = internalUsedLength<Allocator>(
                        nodes, begin, boundary);
                    const auto right = internalUsedLength<Allocator>(
                        nodes, boundary, scan + 1);
                    if (left > pagePayloadBytes || right > pagePayloadBytes) {
                        continue;
                    }
                    const auto difference =
                        left > right ? left - right : right - left;
                    if (difference < selectedDifference) {
                        selected = boundary;
                        selectedDifference = difference;
                    }
                }
                if (selected == 0) {
                    throw DatabaseError{
                        Errc::ResourceLimit,
                        "ordered internal overflow has no valid split boundary"};
                }
                ranges.emplace_back(begin, selected);
                begin = selected;
                ++scan;
            }
            ranges.emplace_back(begin, nodes.size());
            for (const auto [rangeBegin, rangeEnd] : ranges) {
                auto payload = encodeInternalPage<Limits>(
                    nodes, rangeBegin, rangeEnd, session.allocator);
                auto prepared = prepareExtent(payload, 1);
                auto minimum = StoredBytes<Allocator>{
                    typename std::allocator_traits<Allocator>::
                        template rebind_alloc<std::byte>{session.allocator}};
                minimum = nodes[rangeBegin].minimumKey;
                parents.push_back(PreparedTreeNode<Allocator>{
                    prepared.reference,
                    std::move(minimum),
                    nodes[rangeBegin].level + 1});
                extents.push_back(std::move(prepared));
            }
            nodes = std::move(parents);
        }
        root = nodes.front().reference;
    }
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
        highWaterBytes,
        *session.providers);

    bool publicationStarted = false;
    try {
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
        session.state.store(
            DatabaseState::RecoveryRequired, std::memory_order_release);
        throw DatabaseError{
            publicationStarted ? Errc::CommitOutcomeUnknown : Errc::CommitFailed,
            publicationStarted
                ? "commit publication outcome is unknown"
                : "commit failed before publication",
            error.nativeCode()};
    } catch (...) {
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
        session.values = std::move(committedValues);
    }
}

} // namespace miare::detail
