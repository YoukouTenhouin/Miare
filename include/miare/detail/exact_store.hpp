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

using ByteBuffer = std::vector<std::byte>;

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

template<class Values>
[[nodiscard]] inline std::size_t commonPrefixLength(const Values& values) {
    if (values.empty()) {
        return 0;
    }
    const auto& first = values.begin()->first;
    const auto& last = values.rbegin()->first;
    std::size_t length = 0;
    while (length != first.size() && length != last.size() &&
           first[length] == last[length]) {
        ++length;
    }
    return length;
}

template<class Limits, class Values>
[[nodiscard]] inline ByteBuffer encodeLeafPage(const Values& values) {
    constexpr auto ceiling = std::max<std::uint64_t>(
        16U * 1024U, Limits::allocationQuantumBytes);
    constexpr auto payloadBytes = ceiling - ExtentLayout::bytes - authenticationTagBytes;
    ByteBuffer payload(static_cast<std::size_t>(payloadBytes));
    const auto prefixLength = commonPrefixLength(values);
    std::uint64_t used = PageLayout::bytes + prefixLength + values.size() * 8ULL;
    for (const auto& [key, value] : values) {
        if (value.size() > Limits::maxInlineValueBytes) {
            throw DatabaseError{
                Errc::ResourceLimit,
                "overflow values are not available in the exact-operation foundation"};
        }
        used += 4ULL + key.size() - prefixLength + 16ULL + value.size();
    }
    if (used > payload.size()) {
        throw DatabaseError{
            Errc::ResourceLimit,
            "exact-operation transaction exceeds one v1 leaf page"};
    }

    MutableByteView output{payload};
    writeBytes(output, PageLayout::magic, "MIAREPG\0");
    writeLittleEndian<std::uint16_t>(1, output, PageLayout::version);
    writeLittleEndian<std::uint16_t>(1, output, PageLayout::type);
    writeLittleEndian<std::uint32_t>(PageLayout::bytes, output, PageLayout::headerLength);
    writeLittleEndian<std::uint32_t>(1, output, PageLayout::role);
    writeLittleEndian<std::uint32_t>(0, output, PageLayout::level);
    writeLittleEndian<std::uint32_t>(
        static_cast<std::uint32_t>(values.size()), output, PageLayout::entryCount);
    writeLittleEndian<std::uint32_t>(
        static_cast<std::uint32_t>(prefixLength), output, PageLayout::prefixLength);
    const auto slotsOffset = PageLayout::bytes + prefixLength;
    const auto entriesOffset = slotsOffset + values.size() * 8U;
    writeLittleEndian<std::uint32_t>(
        static_cast<std::uint32_t>(slotsOffset), output, PageLayout::slotsOffset);
    writeLittleEndian<std::uint32_t>(
        static_cast<std::uint32_t>(entriesOffset), output, PageLayout::entriesOffset);
    writeLittleEndian<std::uint32_t>(
        static_cast<std::uint32_t>(used), output, PageLayout::usedLength);
    if (!values.empty()) {
        writeBytes(
            output,
            PageLayout::bytes,
            ByteView{values.begin()->first}.first(prefixLength));
    }

    std::size_t slot = slotsOffset;
    std::size_t entry = entriesOffset;
    for (const auto& [key, value] : values) {
        const auto suffixLength = key.size() - prefixLength;
        const auto entryLength = 4U + suffixLength + 16U + value.size();
        writeLittleEndian<std::uint32_t>(
            static_cast<std::uint32_t>(entry), output, slot);
        writeLittleEndian<std::uint32_t>(
            static_cast<std::uint32_t>(entryLength), output, slot + 4);
        writeLittleEndian<std::uint32_t>(
            static_cast<std::uint32_t>(suffixLength), output, entry);
        writeBytes(output, entry + 4, ByteView{key}.subspan(prefixLength));
        const auto representation = entry + 4 + suffixLength;
        output[representation] = std::byte{0};
        writeLittleEndian<std::uint64_t>(
            value.size(), output, representation + 8);
        writeBytes(output, representation + 16, value);
        slot += 8;
        entry += entryLength;
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

struct PreparedExactExtent {
    ExtentReference reference;
    ByteBuffer bytes;
};

template<class Limits, class Values>
[[nodiscard]] inline PreparedExactExtent prepareLeafExtent(
    const Values& values,
    std::uint64_t generation,
    std::uint64_t blockIndex,
    OpenedDatabase& opened,
    ProviderSet& providers) {
    auto decoded = encodeLeafPage<Limits>(values);
    auto& crypto = ProviderAccess::crypto(providers);
    ByteBuffer stored = decoded;
    std::uint32_t flags = 0;
    std::uint32_t codec = 0;
    if (opened.format.compression == Compression::ZStd) {
        auto& compression = ProviderAccess::compression(providers);
        ByteBuffer candidate(compression.compressBound(decoded.size()));
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
    ByteBuffer extent(blockCount * Limits::allocationQuantumBytes);
    MutableByteView output{extent};
    writeBytes(output, ExtentLayout::magic, "MIAREXT\0");
    writeLittleEndian<std::uint16_t>(1, output, ExtentLayout::version);
    writeLittleEndian<std::uint16_t>(2, output, ExtentLayout::unitKind);
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
    return PreparedExactExtent{reference, std::move(extent)};
}

template<class Limits, class Allocator>
[[nodiscard]] inline OrderedKeyValues<Allocator> decodeLeafPage(
    ByteView payload,
    const Allocator& allocator) {
    if (payload.size() !=
            std::max<std::uint64_t>(16U * 1024U, Limits::allocationQuantumBytes) -
                ExtentLayout::bytes - authenticationTagBytes ||
        !matches(payload, PageLayout::magic, "MIAREPG\0") ||
        readLittleEndian<std::uint16_t>(payload, PageLayout::version) != 1 ||
        readLittleEndian<std::uint16_t>(payload, PageLayout::type) != 1 ||
        readLittleEndian<std::uint32_t>(payload, PageLayout::headerLength) !=
            PageLayout::bytes ||
        readLittleEndian<std::uint32_t>(payload, PageLayout::role) != 1 ||
        readLittleEndian<std::uint32_t>(payload, PageLayout::level) != 0 ||
        readLittleEndian<std::uint32_t>(payload, PageLayout::flags) != 0 ||
        !allZero(payload, PageLayout::leftmostChild, PageLayout::bytes)) {
        throwCorrupt("ordered leaf header is invalid");
    }
    const auto count = readLittleEndian<std::uint32_t>(payload, PageLayout::entryCount);
    const auto prefixLength =
        readLittleEndian<std::uint32_t>(payload, PageLayout::prefixLength);
    const auto slotsOffset = readLittleEndian<std::uint32_t>(
        payload, PageLayout::slotsOffset);
    const auto entriesOffset = readLittleEndian<std::uint32_t>(
        payload, PageLayout::entriesOffset);
    const auto usedLength = readLittleEndian<std::uint32_t>(
        payload, PageLayout::usedLength);
    if (slotsOffset != PageLayout::bytes + prefixLength ||
        entriesOffset != slotsOffset + static_cast<std::uint64_t>(count) * 8 ||
        usedLength < entriesOffset || usedLength > payload.size() ||
        !allZero(payload, usedLength, payload.size())) {
        throwCorrupt("ordered leaf bounds are invalid");
    }
    const auto prefix = payload.subspan(PageLayout::bytes, prefixLength);
    auto values = makeOrderedKeyValues(allocator);
    using ByteAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<std::byte>;
    std::size_t expectedEntry = entriesOffset;
    for (std::uint32_t index = 0; index != count; ++index) {
        const auto slot = slotsOffset + index * 8U;
        const auto entryOffset = readLittleEndian<std::uint32_t>(payload, slot);
        const auto entryLength = readLittleEndian<std::uint32_t>(payload, slot + 4);
        if (entryOffset != expectedEntry || entryLength < 20 ||
            entryOffset > usedLength || entryLength > usedLength - entryOffset) {
            throwCorrupt("ordered leaf slot is invalid");
        }
        const auto suffixLength =
            readLittleEndian<std::uint32_t>(payload, entryOffset);
        if (suffixLength > entryLength - 20) {
            throwCorrupt("ordered leaf key suffix is invalid");
        }
        const auto representation = entryOffset + 4 + suffixLength;
        if (payload[representation] != std::byte{0} ||
            !allZero(payload, representation + 1, representation + 8)) {
            throwCorrupt("ordered leaf value representation is invalid");
        }
        const auto valueLength =
            readLittleEndian<std::uint64_t>(payload, representation + 8);
        if (valueLength > Limits::maxInlineValueBytes ||
            entryLength != 20ULL + suffixLength + valueLength) {
            throwCorrupt("ordered leaf value length is invalid");
        }
        StoredBytes<Allocator> key{ByteAllocator{allocator}};
        key.assign(prefix.begin(), prefix.end());
        key.insert(
            key.end(),
            payload.begin() + entryOffset + 4,
            payload.begin() + representation);
        if (key.size() > Limits::maxKeyBytes ||
            (index != 0 && !UnsignedBytesLess{}(values.rbegin()->first, key))) {
            throwCorrupt("ordered leaf keys are not canonical");
        }
        StoredBytes<Allocator> value{ByteAllocator{allocator}};
        value.assign(
            payload.begin() + representation + 16,
            payload.begin() + representation + 16 + valueLength);
        values.emplace(std::move(key), std::move(value));
        expectedEntry += entryLength;
    }
    if (expectedEntry != usedLength || values.empty() ||
        commonPrefixLength(values) != prefixLength) {
        throwCorrupt("ordered leaf image is noncanonical");
    }
    return values;
}

template<class Limits, class Allocator>
[[nodiscard]] inline OrderedKeyValues<Allocator> loadExactValues(
    DurableFile& file,
    OpenedDatabase& opened,
    ProviderSet& providers,
    const Allocator& allocator) {
    const auto reference = decodeExtentReference(opened.format.orderedRoot);
    if (reference.null()) {
        return makeOrderedKeyValues(allocator);
    }
    validateExtentReference<Limits>(
        reference, opened.format.generation, opened.format.highWaterBytes);
    ByteBuffer extent(reference.blockCount * Limits::allocationQuantumBytes);
    const auto offset = reference.blockIndex * Limits::allocationQuantumBytes;
    file.readExactAt(offset, extent);
    const ByteView input{extent};
    if (!matches(input, ExtentLayout::magic, "MIAREXT\0") ||
        readLittleEndian<std::uint16_t>(input, ExtentLayout::version) != 1 ||
        readLittleEndian<std::uint16_t>(input, ExtentLayout::unitKind) != 2 ||
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
        throwCorrupt("ordered leaf extent is noncanonical");
    }
    const auto flags = readLittleEndian<std::uint32_t>(input, ExtentLayout::flags);
    const auto codec = readLittleEndian<std::uint32_t>(input, ExtentLayout::codec);
    const auto codecProfile =
        readLittleEndian<std::uint32_t>(input, ExtentLayout::codecProfile);
    const auto storedLength =
        readLittleEndian<std::uint64_t>(input, ExtentLayout::storedLength);
    const auto decodedLength =
        readLittleEndian<std::uint64_t>(input, ExtentLayout::decodedLength);
    if (flags > 1 || codec != flags || codecProfile != flags ||
        reference.encodedLength !=
            ExtentLayout::bytes + storedLength + authenticationTagBytes ||
        decodedLength !=
            std::max<std::uint64_t>(16U * 1024U, Limits::allocationQuantumBytes) -
                ExtentLayout::bytes - authenticationTagBytes ||
        (flags == 0 && storedLength != decodedLength)) {
        throwCorrupt("ordered leaf extent representation is invalid");
    }
    ByteBuffer stored(storedLength);
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
        throwCorrupt("ordered leaf authentication failed");
    }
    ByteBuffer decoded(decodedLength);
    if (flags == 1) {
        ProviderAccess::compression(providers).decompress(stored, decoded);
    } else {
        decoded = std::move(stored);
    }
    return decodeLeafPage<Limits>(decoded, allocator);
}

struct PreparedPublication {
    PublicationSlot slot;
    PublicationPlaintext plaintext;
    std::uint16_t slotIndex;
};

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
    std::optional<PreparedExactExtent> leaf;
    ExtentReference root;
    std::uint64_t highWaterBytes = session.opened.format.highWaterBytes;
    if (!values.empty()) {
        leaf = prepareLeafExtent<Limits>(
            values,
            generation,
            startBlock,
            session.opened,
            *session.providers);
        if (leaf->bytes.size() > Limits::maxFileGrowthPerTransaction ||
            leaf->bytes.size() > Limits::maxDatabaseBytes ||
            session.opened.format.highWaterBytes >
                Limits::maxDatabaseBytes - leaf->bytes.size()) {
            throw DatabaseError{
                Errc::ResourceLimit,
                "commit would exceed the capacity profile"};
        }
        root = leaf->reference;
        highWaterBytes += leaf->bytes.size();
    }
    auto publication = prepareExactPublication<Limits>(
        session.opened,
        root,
        highWaterBytes,
        *session.providers);

    bool publicationStarted = false;
    try {
        if (leaf) {
            session.file->writeExactAt(
                root.blockIndex * Limits::allocationQuantumBytes,
                leaf->bytes);
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

    session.opened.publication = std::move(publication.plaintext);
    session.opened.format.generation = generation;
    session.opened.format.highWaterBytes = highWaterBytes;
    session.opened.format.orderedRoot = encodeExtentReference(root);
    session.values = std::move(committedValues);
}

} // namespace miare::detail
