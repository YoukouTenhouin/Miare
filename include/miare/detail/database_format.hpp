#pragma once

#include <miare/detail/blake2b.hpp>
#include <miare/detail/durable_file.hpp>
#include <miare/detail/providers.hpp>
#include <miare/result.hpp>
#include <miare/types.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>

namespace miare::detail {

inline constexpr std::size_t bootstrapBytes = 4096;
inline constexpr std::size_t publicationSlotBytes = 4096;
inline constexpr std::size_t publicationEnvelopeBytes = 64;
inline constexpr std::size_t publicationPlaintextBytes = 4016;
inline constexpr std::size_t commonRegionBytes = 64U * 1024U;
inline constexpr std::uint32_t commonFormatVersion = 1;
inline constexpr std::uint32_t btreeBackendIdentifier = 1;
inline constexpr std::uint32_t unencryptedSuiteIdentifier = 0;
inline constexpr std::uint32_t xchachaSuiteIdentifier = 1;
inline constexpr std::uint32_t blake2bKdfIdentifier = 1;
inline constexpr std::uint32_t derivationVersion = 1;
inline constexpr std::uint32_t capacityProfileVersion = 1;
inline constexpr std::size_t capacityProfileBytes = 104;

struct BootstrapLayout {
    static constexpr std::size_t magic = 0;
    static constexpr std::size_t envelopeVersion = 8;
    static constexpr std::size_t reserved16 = 10;
    static constexpr std::size_t length = 12;
    static constexpr std::size_t commonFormat = 16;
    static constexpr std::size_t requiredFeatures = 20;
    static constexpr std::size_t kdf = 24;
    static constexpr std::size_t derivation = 28;
    static constexpr std::size_t encryptionSuite = 32;
    static constexpr std::size_t reserved32 = 36;
    static constexpr std::size_t databaseIdentity = 40;
    static constexpr std::size_t salt = 56;
    static constexpr std::size_t commonRegionLength = 72;
    static constexpr std::size_t slotLength = 80;
    static constexpr std::size_t slotCount = 84;
    static constexpr std::size_t backendDataOffset = 88;
    static constexpr std::size_t reserved = 96;
};

struct SlotEnvelopeLayout {
    static constexpr std::size_t magic = 0;
    static constexpr std::size_t version = 8;
    static constexpr std::size_t index = 10;
    static constexpr std::size_t length = 12;
    static constexpr std::size_t ciphertextLength = 16;
    static constexpr std::size_t flags = 20;
    static constexpr std::size_t nonce = 24;
    static constexpr std::size_t reserved = 48;
    static constexpr std::size_t ciphertext = 64;
    static constexpr std::size_t tag = 4080;
};

struct PublicationLayout {
    static constexpr std::size_t magic = 0;
    static constexpr std::size_t version = 8;
    static constexpr std::size_t slotIndex = 10;
    static constexpr std::size_t length = 12;
    static constexpr std::size_t generation = 16;
    static constexpr std::size_t predecessorGeneration = 24;
    static constexpr std::size_t databaseIdentity = 32;
    static constexpr std::size_t commonFormat = 48;
    static constexpr std::size_t storageBackend = 52;
    static constexpr std::size_t backendFormat = 56;
    static constexpr std::size_t encryptionSuite = 60;
    static constexpr std::size_t requiredFeatures = 64;
    static constexpr std::size_t optionalFeatures = 72;
    static constexpr std::size_t compression = 80;
    static constexpr std::size_t codec = 84;
    static constexpr std::size_t codecProfile = 88;
    static constexpr std::size_t capacityProfileVersion = 92;
    static constexpr std::size_t capacityProfileLength = 96;
    static constexpr std::size_t reserved32 = 100;
    static constexpr std::size_t capacityProfileDigest = 104;
    static constexpr std::size_t capacityProfile = 136;
    static constexpr std::size_t orderedRoot = 240;
    static constexpr std::size_t blobRoot = 272;
    static constexpr std::size_t allocatorRoot = 304;
    static constexpr std::size_t highWaterBlocks = 336;
    static constexpr std::size_t flags = 344;
    static constexpr std::size_t reserved = 352;
};

using Bootstrap = std::array<std::byte, bootstrapBytes>;
using PublicationSlot = std::array<std::byte, publicationSlotBytes>;
using PublicationPlaintext = std::array<std::byte, publicationPlaintextBytes>;
using CommonRegion = std::array<std::byte, commonRegionBytes>;

template<class T>
requires std::is_unsigned_v<T>
inline void writeLittleEndian(T value, MutableByteView output, std::size_t offset) {
    for (std::size_t index = 0; index != sizeof(T); ++index) {
        output[offset + index] = std::byte{
            static_cast<unsigned char>(value >> (index * 8U))};
    }
}

template<class T>
requires std::is_unsigned_v<T>
[[nodiscard]] inline T readLittleEndian(ByteView input, std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index != sizeof(T); ++index) {
        value |= static_cast<std::uint64_t>(
                     std::to_integer<unsigned char>(input[offset + index]))
            << (index * 8U);
    }
    return static_cast<T>(value);
}

template<std::size_t Size>
inline void writeBytes(
    MutableByteView output,
    std::size_t offset,
    const char (&bytes)[Size]) {
    static_assert(Size > 0);
    std::copy_n(
        reinterpret_cast<const std::byte*>(bytes),
        Size - 1,
        output.begin() + static_cast<std::ptrdiff_t>(offset));
}

inline void writeBytes(MutableByteView output, std::size_t offset, ByteView bytes) {
    std::copy(
        bytes.begin(),
        bytes.end(),
        output.begin() + static_cast<std::ptrdiff_t>(offset));
}

template<std::size_t Size>
[[nodiscard]] inline bool matches(
    ByteView input,
    std::size_t offset,
    const char (&bytes)[Size]) {
    static_assert(Size > 0);
    return std::equal(
        input.begin() + static_cast<std::ptrdiff_t>(offset),
        input.begin() + static_cast<std::ptrdiff_t>(offset + Size - 1),
        reinterpret_cast<const std::byte*>(bytes));
}

[[nodiscard]] inline bool allZero(ByteView input, std::size_t begin, std::size_t end) {
    return std::all_of(
        input.begin() + static_cast<std::ptrdiff_t>(begin),
        input.begin() + static_cast<std::ptrdiff_t>(end),
        [](std::byte byte) { return byte == std::byte{0}; });
}

[[nodiscard]] inline bool constantTimeEqual(ByteView left, ByteView right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    unsigned char difference = 0;
    for (std::size_t index = 0; index != left.size(); ++index) {
        difference |= std::to_integer<unsigned char>(left[index] ^ right[index]);
    }
    return difference == 0;
}

class Secret32 {
public:
    Secret32() = default;
    Secret32(const Secret32&) = delete;
    Secret32& operator=(const Secret32&) = delete;

    Secret32(Secret32&& other) noexcept : bytes_(other.bytes_) {
        other.erase();
    }

    Secret32& operator=(Secret32&&) = delete;

    ~Secret32() { erase(); }

    void erase() noexcept {
        volatile std::byte* cursor = bytes_.data();
        for (std::size_t index = 0; index != bytes_.size(); ++index) {
            cursor[index] = std::byte{0};
        }
    }

    [[nodiscard]] ByteView view() const noexcept { return bytes_; }
    [[nodiscard]] MutableByteView mutableView() noexcept { return bytes_; }

private:
    std::array<std::byte, cryptoKeyBytes> bytes_{};
};

struct SessionKeys {
    Secret32 header;
    Secret32 mainData;
    Secret32 recovery;
    Secret32 blob;
};

template<class Limits>
[[nodiscard]] inline std::array<std::byte, capacityProfileBytes>
encodeCapacityProfile() {
    std::array<std::byte, capacityProfileBytes> profile{};
    MutableByteView output{profile};
    writeLittleEndian<std::uint64_t>(Limits::allocationQuantumBytes, output, 0);
    writeLittleEndian<std::uint64_t>(Limits::maxInlineValueBytes, output, 8);
    writeLittleEndian<std::uint64_t>(Limits::blobChunkBytes, output, 16);
    writeLittleEndian<std::uint64_t>(Limits::maxKeyBytes, output, 24);
    writeLittleEndian<std::uint64_t>(Limits::maxValueBytes, output, 32);
    writeLittleEndian<std::uint64_t>(Limits::maxBlobBytes, output, 40);
    writeLittleEndian<std::uint64_t>(Limits::maxDatabaseBytes, output, 48);
    writeLittleEndian<std::uint64_t>(
        Limits::maxKeyMutationsPerTransaction, output, 56);
    writeLittleEndian<std::uint64_t>(
        Limits::maxBlobMutationsPerTransaction, output, 64);
    writeLittleEndian<std::uint64_t>(
        Limits::maxBlobBytesPerTransaction, output, 72);
    writeLittleEndian<std::uint64_t>(
        Limits::maxFileGrowthPerTransaction, output, 80);
    writeLittleEndian<std::uint32_t>(Limits::maxCursorsPerTransaction, output, 88);
    writeLittleEndian<std::uint32_t>(Limits::maxBlobReadersPerTransaction, output, 92);
    writeLittleEndian<std::uint32_t>(
        Limits::maxOpenBlobWritersPerTransaction, output, 96);
    return profile;
}

[[nodiscard]] inline std::array<std::byte, cryptoKeyBytes>
capacityProfileDigest(CryptoProvider& crypto, ByteView profile) {
    std::array<std::byte, 128> input{};
    writeBytes(input, 0, "MiareLimitsV1");
    writeLittleEndian<std::uint32_t>(capacityProfileVersion, input, 16);
    writeLittleEndian<std::uint32_t>(capacityProfileBytes, input, 20);
    writeBytes(input, 24, profile);
    std::array<std::byte, cryptoKeyBytes> digest{};
    crypto.hashBlake2b256(input, digest);
    return digest;
}

[[nodiscard]] inline std::array<std::byte, cryptoKeyBytes>
capacityProfileDigest(ByteView profile) {
    std::array<std::byte, 24> prefix{};
    writeBytes(prefix, 0, "MiareLimitsV1");
    writeLittleEndian<std::uint32_t>(capacityProfileVersion, prefix, 16);
    writeLittleEndian<std::uint32_t>(capacityProfileBytes, prefix, 20);
    return blake2b<cryptoKeyBytes>(prefix, profile);
}

template<class Limits>
[[nodiscard]] inline std::array<std::byte, cryptoKeyBytes>
capacityProfileDigest(CryptoProvider& crypto) {
    const auto profile = encodeCapacityProfile<Limits>();
    return capacityProfileDigest(crypto, profile);
}


template<class Limits>
[[nodiscard]] inline std::array<std::byte, cryptoKeyBytes>
capacityProfileDigest() {
    const auto profile = encodeCapacityProfile<Limits>();
    return capacityProfileDigest(profile);
}

inline void requireCallerKey(EncryptionKeyView key) {
    if (key.bytes().size() != cryptoKeyBytes) {
        throw ContractError{
            Errc::InvalidArgument,
            "XChaCha20-Poly1305-IETF requires 32-byte key material"};
    }
}

[[nodiscard]] inline std::uint32_t compressionIdentifier(Compression compression) {
    switch (compression) {
    case Compression::None:
        return 0;
    case Compression::ZStd:
        return 1;
    }
    throw ContractError{Errc::InvalidConfiguration, "unsupported compression choice"};
}

[[nodiscard]] constexpr std::uint32_t encryptionSuiteIdentifier(
    EncryptionSuite encryptionSuite) {
    switch (encryptionSuite) {
    case EncryptionSuite::None:
        return 0;
    case EncryptionSuite::XChaCha20Poly1305Ietf:
        return 1;
    }
    throw ContractError{
        Errc::InvalidConfiguration, "unsupported encryption suite"};
}

inline void validateCreateOptions(const CreateOptions& options) {
    if (options.storageBackend != StorageBackend::BTree) {
        throw ContractError{Errc::InvalidConfiguration, "unsupported storage backend"};
    }
    if (options.encryptionSuite == EncryptionSuite::None) {
        throw DatabaseError{
            Errc::UnexpectedKey,
            "suite 0 must be created through the keyless entry point"};
    }
    if (options.encryptionSuite != EncryptionSuite::XChaCha20Poly1305Ietf) {
        throw ContractError{Errc::InvalidConfiguration, "unsupported encryption suite"};
    }
    (void)compressionIdentifier(options.compression);
}

inline void validateCreateOptions(const UnencryptedCreateOptions& options) {
    if (options.storageBackend != StorageBackend::BTree) {
        throw ContractError{Errc::InvalidConfiguration, "unsupported storage backend"};
    }
    (void)compressionIdentifier(options.compression);
}

[[nodiscard]] inline SessionKeys deriveSessionKeys(
    CryptoProvider& crypto,
    EncryptionKeyView callerKey,
    ByteView databaseIdentity,
    ByteView salt) {
    Secret32 databaseRoot;
    SessionKeys keys;
    crypto.deriveDatabaseRoot(
        callerKey.bytes(),
        databaseIdentity,
        salt,
        xchachaSuiteIdentifier,
        derivationVersion,
        databaseRoot.mutableView());
    crypto.deriveSubkey(databaseRoot.view(), 1, keys.header.mutableView());
    crypto.deriveSubkey(databaseRoot.view(), 2, keys.mainData.mutableView());
    crypto.deriveSubkey(databaseRoot.view(), 3, keys.recovery.mutableView());
    crypto.deriveSubkey(databaseRoot.view(), 4, keys.blob.mutableView());
    return keys;
}

[[nodiscard]] inline std::array<std::byte, 4176> publicationAssociatedData(
    const Bootstrap& bootstrap,
    ByteView envelope) {
    std::array<std::byte, 4176> associatedData{};
    writeBytes(associatedData, 0, "MiareHeaderV1");
    writeBytes(associatedData, 16, bootstrap);
    writeBytes(associatedData, 4112, envelope);
    return associatedData;
}

[[nodiscard]] inline std::array<std::byte, authenticationTagBytes>
publicationChecksum(
    const Bootstrap& bootstrap,
    ByteView envelope,
    const PublicationPlaintext& plaintext) {
    constexpr std::array<std::byte, 16> domain{
        std::byte{'M'}, std::byte{'i'}, std::byte{'a'}, std::byte{'r'},
        std::byte{'e'}, std::byte{'S'}, std::byte{'l'}, std::byte{'o'},
        std::byte{'t'}, std::byte{'C'}, std::byte{'h'}, std::byte{'e'},
        std::byte{'c'}, std::byte{'k'}, std::byte{'V'}, std::byte{'1'}};
    return blake2b<authenticationTagBytes>(domain, bootstrap, envelope, plaintext);
}

template<class Limits>
[[nodiscard]] inline PublicationPlaintext makeInitialPublicationPlaintext(
    std::uint16_t slotIndex,
    ByteView databaseIdentity,
    Compression compression,
    EncryptionSuite encryptionSuite,
    CryptoProvider* crypto) {
    PublicationPlaintext plaintext{};
    MutableByteView output{plaintext};
    writeBytes(output, PublicationLayout::magic, "MIAREPUB");
    writeLittleEndian<std::uint16_t>(1, output, PublicationLayout::version);
    writeLittleEndian<std::uint16_t>(slotIndex, output, PublicationLayout::slotIndex);
    writeLittleEndian<std::uint32_t>(
        publicationPlaintextBytes, output, PublicationLayout::length);
    writeLittleEndian<std::uint64_t>(1, output, PublicationLayout::generation);
    writeLittleEndian<std::uint64_t>(
        0, output, PublicationLayout::predecessorGeneration);
    writeBytes(output, PublicationLayout::databaseIdentity, databaseIdentity);
    writeLittleEndian<std::uint32_t>(
        commonFormatVersion, output, PublicationLayout::commonFormat);
    writeLittleEndian<std::uint32_t>(
        btreeBackendIdentifier, output, PublicationLayout::storageBackend);
    writeLittleEndian<std::uint32_t>(1, output, PublicationLayout::backendFormat);
    writeLittleEndian<std::uint32_t>(
        encryptionSuiteIdentifier(encryptionSuite),
        output,
        PublicationLayout::encryptionSuite);
    const auto compressionId = compressionIdentifier(compression);
    writeLittleEndian<std::uint32_t>(
        compressionId, output, PublicationLayout::compression);
    writeLittleEndian<std::uint32_t>(compressionId, output, PublicationLayout::codec);
    writeLittleEndian<std::uint32_t>(
        compressionId, output, PublicationLayout::codecProfile);
    writeLittleEndian<std::uint32_t>(
        capacityProfileVersion, output, PublicationLayout::capacityProfileVersion);
    writeLittleEndian<std::uint32_t>(
        capacityProfileBytes, output, PublicationLayout::capacityProfileLength);
    const auto digest = crypto
        ? capacityProfileDigest<Limits>(*crypto)
        : capacityProfileDigest<Limits>();
    writeBytes(output, PublicationLayout::capacityProfileDigest, digest);
    const auto profile = encodeCapacityProfile<Limits>();
    writeBytes(output, PublicationLayout::capacityProfile, profile);
    writeLittleEndian<std::uint64_t>(
        commonRegionBytes / Limits::allocationQuantumBytes,
        output,
        PublicationLayout::highWaterBlocks);
    return plaintext;
}

template<class Limits>
[[nodiscard]] inline CommonRegion makeInitialCommonRegion(
    EncryptionKeyView callerKey,
    CryptoProvider& crypto,
    Compression compression) {
    CommonRegion region{};
    Bootstrap bootstrap{};
    std::array<std::byte, databaseIdentityBytes> databaseIdentity{};
    std::array<std::byte, kdfSaltBytes> salt{};
    crypto.randomBytes(databaseIdentity);
    crypto.randomBytes(salt);

    MutableByteView bootstrapOutput{bootstrap};
    writeBytes(bootstrapOutput, BootstrapLayout::magic, "MIAREDB\0");
    writeLittleEndian<std::uint16_t>(
        1, bootstrapOutput, BootstrapLayout::envelopeVersion);
    writeLittleEndian<std::uint32_t>(
        bootstrapBytes, bootstrapOutput, BootstrapLayout::length);
    writeLittleEndian<std::uint32_t>(
        commonFormatVersion, bootstrapOutput, BootstrapLayout::commonFormat);
    writeLittleEndian<std::uint32_t>(
        blake2bKdfIdentifier, bootstrapOutput, BootstrapLayout::kdf);
    writeLittleEndian<std::uint32_t>(
        derivationVersion, bootstrapOutput, BootstrapLayout::derivation);
    writeLittleEndian<std::uint32_t>(
        xchachaSuiteIdentifier, bootstrapOutput, BootstrapLayout::encryptionSuite);
    writeBytes(bootstrapOutput, BootstrapLayout::databaseIdentity, databaseIdentity);
    writeBytes(bootstrapOutput, BootstrapLayout::salt, salt);
    writeLittleEndian<std::uint64_t>(
        commonRegionBytes, bootstrapOutput, BootstrapLayout::commonRegionLength);
    writeLittleEndian<std::uint32_t>(
        publicationSlotBytes, bootstrapOutput, BootstrapLayout::slotLength);
    writeLittleEndian<std::uint32_t>(2, bootstrapOutput, BootstrapLayout::slotCount);
    writeLittleEndian<std::uint64_t>(
        commonRegionBytes, bootstrapOutput, BootstrapLayout::backendDataOffset);
    writeBytes(region, 0, bootstrap);

    auto keys = deriveSessionKeys(
        crypto, callerKey, databaseIdentity, salt);
    for (std::uint16_t slotIndex = 0; slotIndex != 2; ++slotIndex) {
        PublicationSlot slot{};
        MutableByteView slotOutput{slot};
        writeBytes(slotOutput, SlotEnvelopeLayout::magic, "MIARESLT");
        writeLittleEndian<std::uint16_t>(1, slotOutput, SlotEnvelopeLayout::version);
        writeLittleEndian<std::uint16_t>(
            slotIndex, slotOutput, SlotEnvelopeLayout::index);
        writeLittleEndian<std::uint32_t>(
            publicationEnvelopeBytes, slotOutput, SlotEnvelopeLayout::length);
        writeLittleEndian<std::uint32_t>(
            publicationPlaintextBytes,
            slotOutput,
            SlotEnvelopeLayout::ciphertextLength);
        std::array<std::byte, aeadNonceBytes> nonce{};
        crypto.randomBytes(nonce);
        writeBytes(slotOutput, SlotEnvelopeLayout::nonce, nonce);
        const auto plaintext = makeInitialPublicationPlaintext<Limits>(
            slotIndex,
            databaseIdentity,
            compression,
            EncryptionSuite::XChaCha20Poly1305Ietf,
            &crypto);
        const auto associatedData = publicationAssociatedData(
            bootstrap, ByteView{slot}.first(publicationEnvelopeBytes));
        crypto.encryptDetached(
            keys.header.view(),
            nonce,
            plaintext,
            associatedData,
            MutableByteView{slot}.subspan(
                SlotEnvelopeLayout::ciphertext, publicationPlaintextBytes),
            MutableByteView{slot}.subspan(
                SlotEnvelopeLayout::tag, authenticationTagBytes));
        writeBytes(
            region,
            bootstrapBytes + slotIndex * publicationSlotBytes,
            slot);
    }
    return region;
}

template<class Limits>
[[nodiscard]] inline CommonRegion makeInitialUnencryptedCommonRegion(
    EntropySource& entropy,
    Compression compression) {
    CommonRegion region{};
    Bootstrap bootstrap{};
    std::array<std::byte, databaseIdentityBytes> databaseIdentity{};
    entropy.randomBytes(databaseIdentity);

    MutableByteView bootstrapOutput{bootstrap};
    writeBytes(bootstrapOutput, BootstrapLayout::magic, "MIAREDB\0");
    writeLittleEndian<std::uint16_t>(
        1, bootstrapOutput, BootstrapLayout::envelopeVersion);
    writeLittleEndian<std::uint32_t>(
        bootstrapBytes, bootstrapOutput, BootstrapLayout::length);
    writeLittleEndian<std::uint32_t>(
        commonFormatVersion, bootstrapOutput, BootstrapLayout::commonFormat);
    writeLittleEndian<std::uint32_t>(
        unencryptedSuiteIdentifier,
        bootstrapOutput,
        BootstrapLayout::encryptionSuite);
    writeBytes(bootstrapOutput, BootstrapLayout::databaseIdentity, databaseIdentity);
    writeLittleEndian<std::uint64_t>(
        commonRegionBytes, bootstrapOutput, BootstrapLayout::commonRegionLength);
    writeLittleEndian<std::uint32_t>(
        publicationSlotBytes, bootstrapOutput, BootstrapLayout::slotLength);
    writeLittleEndian<std::uint32_t>(2, bootstrapOutput, BootstrapLayout::slotCount);
    writeLittleEndian<std::uint64_t>(
        commonRegionBytes, bootstrapOutput, BootstrapLayout::backendDataOffset);
    writeBytes(region, 0, bootstrap);

    for (std::uint16_t slotIndex = 0; slotIndex != 2; ++slotIndex) {
        PublicationSlot slot{};
        MutableByteView slotOutput{slot};
        writeBytes(slotOutput, SlotEnvelopeLayout::magic, "MIARESLT");
        writeLittleEndian<std::uint16_t>(1, slotOutput, SlotEnvelopeLayout::version);
        writeLittleEndian<std::uint16_t>(
            slotIndex, slotOutput, SlotEnvelopeLayout::index);
        writeLittleEndian<std::uint32_t>(
            publicationEnvelopeBytes, slotOutput, SlotEnvelopeLayout::length);
        writeLittleEndian<std::uint32_t>(
            publicationPlaintextBytes,
            slotOutput,
            SlotEnvelopeLayout::ciphertextLength);
        const auto plaintext = makeInitialPublicationPlaintext<Limits>(
            slotIndex,
            databaseIdentity,
            compression,
            EncryptionSuite::None,
            nullptr);
        writeBytes(slotOutput, SlotEnvelopeLayout::ciphertext, plaintext);
        const auto checksum = publicationChecksum(
            bootstrap,
            ByteView{slot}.first(publicationEnvelopeBytes),
            plaintext);
        writeBytes(slotOutput, SlotEnvelopeLayout::tag, checksum);
        writeBytes(region, bootstrapBytes + slotIndex * publicationSlotBytes, slot);
    }
    return region;
}

struct OpenedFormat {
    Compression compression;
    EncryptionSuite encryptionSuite;
    std::uint64_t generation;
    std::uint64_t highWaterBytes;
    std::uint64_t optionalFeatures;
    std::array<std::byte, 32> orderedRoot;
    std::array<std::byte, 32> blobRoot;
    std::array<std::byte, 32> allocatorRoot;
};

struct OpenedDatabase {
    OpenedFormat format;
    std::optional<SessionKeys> keys;
    Bootstrap bootstrap;
    PublicationPlaintext publication;
    bool rejectedInactivePublication = false;
    std::uint64_t abandonedTailBytes = 0;
};

struct SelectedPublication {
    OpenedFormat format;
    Bootstrap bootstrap;
    PublicationPlaintext publication;
    bool rejectedInactivePublication = false;
    std::uint64_t abandonedTailBytes = 0;
};

[[noreturn]] inline void throwCorrupt(const char* message) {
    throw DatabaseError{Errc::Corrupt, message};
}

inline void validateVisibleBootstrapDispatch(const Bootstrap& bootstrap) {
    const ByteView input{bootstrap};
    if (!matches(input, BootstrapLayout::magic, "MIAREDB\0") ||
        readLittleEndian<std::uint16_t>(input, BootstrapLayout::envelopeVersion) != 1 ||
        readLittleEndian<std::uint32_t>(input, BootstrapLayout::length) !=
            bootstrapBytes ||
        readLittleEndian<std::uint32_t>(input, BootstrapLayout::commonFormat) !=
            commonFormatVersion) {
        throw DatabaseError{Errc::UnsupportedFormat, "unsupported database envelope"};
    }
    const auto suite = readLittleEndian<std::uint32_t>(
        input, BootstrapLayout::encryptionSuite);
    if (readLittleEndian<std::uint32_t>(input, BootstrapLayout::requiredFeatures) !=
            0 ||
        suite > xchachaSuiteIdentifier) {
        throw DatabaseError{Errc::UnsupportedFeature, "unsupported database feature"};
    }
    const auto expectedKdf = suite == unencryptedSuiteIdentifier
        ? unencryptedSuiteIdentifier
        : blake2bKdfIdentifier;
    const auto expectedDerivation = suite == unencryptedSuiteIdentifier
        ? unencryptedSuiteIdentifier
        : derivationVersion;
    if (readLittleEndian<std::uint32_t>(input, BootstrapLayout::kdf) != expectedKdf ||
        readLittleEndian<std::uint32_t>(input, BootstrapLayout::derivation) !=
            expectedDerivation) {
        throw DatabaseError{Errc::IncompatibleProfile, "unsupported key derivation profile"};
    }
}

[[nodiscard]] inline EncryptionSuite visibleEncryptionSuite(
    const Bootstrap& bootstrap) noexcept {
    return readLittleEndian<std::uint32_t>(
               bootstrap, BootstrapLayout::encryptionSuite) ==
            unencryptedSuiteIdentifier
        ? EncryptionSuite::None
        : EncryptionSuite::XChaCha20Poly1305Ietf;
}

[[nodiscard]] inline bool canonicalSlotEnvelope(
    const PublicationSlot& slot,
    std::uint16_t physicalIndex) {
    const ByteView input{slot};
    return matches(input, SlotEnvelopeLayout::magic, "MIARESLT") &&
        readLittleEndian<std::uint16_t>(input, SlotEnvelopeLayout::version) == 1 &&
        readLittleEndian<std::uint16_t>(input, SlotEnvelopeLayout::index) ==
            physicalIndex &&
        readLittleEndian<std::uint32_t>(input, SlotEnvelopeLayout::length) ==
            publicationEnvelopeBytes &&
        readLittleEndian<std::uint32_t>(
            input, SlotEnvelopeLayout::ciphertextLength) == publicationPlaintextBytes &&
        readLittleEndian<std::uint32_t>(input, SlotEnvelopeLayout::flags) == 0 &&
        allZero(input, SlotEnvelopeLayout::reserved, SlotEnvelopeLayout::ciphertext);
}

[[nodiscard]] inline std::optional<PublicationPlaintext> authenticateSlot(
    const PublicationSlot& slot,
    std::uint16_t physicalIndex,
    const Bootstrap& bootstrap,
    CryptoProvider& crypto,
    const Secret32& headerKey) {
    if (!canonicalSlotEnvelope(slot, physicalIndex)) {
        return std::nullopt;
    }
    const ByteView input{slot};
    const auto associatedData = publicationAssociatedData(
        bootstrap, input.first(publicationEnvelopeBytes));
    PublicationPlaintext plaintext{};
    if (!crypto.decryptDetached(
            headerKey.view(),
            input.subspan(SlotEnvelopeLayout::nonce, aeadNonceBytes),
            input.subspan(
                SlotEnvelopeLayout::ciphertext, publicationPlaintextBytes),
            input.subspan(SlotEnvelopeLayout::tag, authenticationTagBytes),
            associatedData,
            plaintext)) {
        return std::nullopt;
    }
    return plaintext;
}

[[nodiscard]] inline std::optional<PublicationPlaintext> checksumSlot(
    const PublicationSlot& slot,
    std::uint16_t physicalIndex,
    const Bootstrap& bootstrap) {
    if (!canonicalSlotEnvelope(slot, physicalIndex) ||
        !allZero(slot, SlotEnvelopeLayout::nonce, SlotEnvelopeLayout::reserved)) {
        return std::nullopt;
    }
    PublicationPlaintext plaintext{};
    std::copy_n(
        slot.begin() + SlotEnvelopeLayout::ciphertext,
        plaintext.size(),
        plaintext.begin());
    const auto expected = publicationChecksum(
        bootstrap,
        ByteView{slot}.first(publicationEnvelopeBytes),
        plaintext);
    if (!constantTimeEqual(
            expected,
            ByteView{slot}.subspan(
                SlotEnvelopeLayout::tag, authenticationTagBytes))) {
        return std::nullopt;
    }
    return plaintext;
}

template<class Limits>
[[nodiscard]] inline OpenedFormat validatePublication(
    const PublicationPlaintext& plaintext,
    std::uint16_t physicalIndex,
    const Bootstrap& bootstrap,
    CryptoProvider* crypto,
    ProviderSet& providers) {
    const ByteView input{plaintext};
    if (!matches(input, PublicationLayout::magic, "MIAREPUB") ||
        readLittleEndian<std::uint16_t>(input, PublicationLayout::version) != 1 ||
        readLittleEndian<std::uint16_t>(input, PublicationLayout::slotIndex) !=
            physicalIndex ||
        readLittleEndian<std::uint32_t>(input, PublicationLayout::length) !=
            publicationPlaintextBytes ||
        readLittleEndian<std::uint64_t>(input, PublicationLayout::generation) == 0 ||
        !allZero(
            input,
            PublicationLayout::reserved32,
            PublicationLayout::capacityProfileDigest) ||
        !allZero(input, PublicationLayout::reserved, publicationPlaintextBytes)) {
        throwCorrupt("publication slot is noncanonical");
    }
    const auto generation = readLittleEndian<std::uint64_t>(
        input, PublicationLayout::generation);
    const auto predecessor = readLittleEndian<std::uint64_t>(
        input, PublicationLayout::predecessorGeneration);
    if ((generation == 1 && predecessor != 0) ||
        (generation > 1 && predecessor != generation - 1) ||
        (generation > 1 && physicalIndex != generation % 2)) {
        throwCorrupt("publication generation relationship is invalid");
    }
    if (!std::equal(
            input.begin() + PublicationLayout::databaseIdentity,
            input.begin() + PublicationLayout::commonFormat,
            bootstrap.begin() + BootstrapLayout::databaseIdentity)) {
        throwCorrupt("publication identity contradicts its bootstrap");
    }
    if (readLittleEndian<std::uint32_t>(input, PublicationLayout::commonFormat) !=
        commonFormatVersion) {
        throw DatabaseError{Errc::UnsupportedFormat, "unsupported common format"};
    }
    const auto encryptionSuite = visibleEncryptionSuite(bootstrap);
    if (readLittleEndian<std::uint32_t>(input, PublicationLayout::encryptionSuite) !=
        encryptionSuiteIdentifier(encryptionSuite)) {
        throwCorrupt("publication protection contradicts its bootstrap");
    }
    if (readLittleEndian<std::uint32_t>(input, PublicationLayout::storageBackend) !=
            btreeBackendIdentifier ||
        readLittleEndian<std::uint64_t>(input, PublicationLayout::requiredFeatures) != 0 ||
        readLittleEndian<std::uint64_t>(input, PublicationLayout::optionalFeatures) != 0) {
        throw DatabaseError{Errc::UnsupportedFeature, "unsupported publication feature"};
    }
    if (readLittleEndian<std::uint32_t>(input, PublicationLayout::backendFormat) != 1) {
        throw DatabaseError{Errc::UnsupportedFormat, "unsupported backend format"};
    }
    const auto compressionId = readLittleEndian<std::uint32_t>(
        input, PublicationLayout::compression);
    if (compressionId > 1) {
        throw DatabaseError{Errc::UnsupportedFeature, "unsupported compression policy"};
    }
    if (readLittleEndian<std::uint32_t>(input, PublicationLayout::codec) !=
            compressionId ||
        readLittleEndian<std::uint32_t>(input, PublicationLayout::codecProfile) !=
            compressionId) {
        throw DatabaseError{Errc::IncompatibleProfile, "incompatible codec profile"};
    }
    if (compressionId == 1) {
        (void)ProviderAccess::compression(providers);
    }
    if (readLittleEndian<std::uint32_t>(
            input, PublicationLayout::capacityProfileVersion) !=
            capacityProfileVersion ||
        readLittleEndian<std::uint32_t>(
            input, PublicationLayout::capacityProfileLength) != capacityProfileBytes) {
        throw DatabaseError{Errc::IncompatibleProfile, "incompatible capacity profile"};
    }
    const auto storedProfile = input.subspan(
        PublicationLayout::capacityProfile, capacityProfileBytes);
    const auto canonicalDigest = crypto
        ? capacityProfileDigest(*crypto, storedProfile)
        : capacityProfileDigest(storedProfile);
    const auto expectedProfile = encodeCapacityProfile<Limits>();
    if (!std::equal(
            canonicalDigest.begin(),
            canonicalDigest.end(),
            input.begin() + PublicationLayout::capacityProfileDigest)) {
        throwCorrupt("capacity profile digest is invalid");
    }
    if (!std::equal(expectedProfile.begin(), expectedProfile.end(), storedProfile.begin())) {
        throw DatabaseError{Errc::IncompatibleProfile, "incompatible capacity profile"};
    }
    const auto highWaterBlocks = readLittleEndian<std::uint64_t>(
        input, PublicationLayout::highWaterBlocks);
    const auto commonRegionBlocks =
        commonRegionBytes / Limits::allocationQuantumBytes;
    if ((generation == 1 &&
         (!allZero(
              input,
              PublicationLayout::orderedRoot,
              PublicationLayout::highWaterBlocks) ||
          highWaterBlocks != commonRegionBlocks)) ||
        (generation > 1 &&
         (highWaterBlocks < commonRegionBlocks ||
          highWaterBlocks > Limits::maxDatabaseBytes /
              Limits::allocationQuantumBytes))) {
        throwCorrupt("database has an invalid committed boundary");
    }
    if (readLittleEndian<std::uint64_t>(input, PublicationLayout::flags) != 0) {
        throwCorrupt("publication flags are noncanonical");
    }
    std::array<std::byte, 32> orderedRoot{};
    std::copy_n(
        input.begin() + PublicationLayout::orderedRoot,
        orderedRoot.size(),
        orderedRoot.begin());
    std::array<std::byte, 32> blobRoot{};
    std::copy_n(
        input.begin() + PublicationLayout::blobRoot,
        blobRoot.size(),
        blobRoot.begin());
    std::array<std::byte, 32> allocatorRoot{};
    std::copy_n(
        input.begin() + PublicationLayout::allocatorRoot,
        allocatorRoot.size(),
        allocatorRoot.begin());
    return OpenedFormat{
        compressionId == 0 ? Compression::None : Compression::ZStd,
        encryptionSuite,
        generation,
        highWaterBlocks * Limits::allocationQuantumBytes,
        readLittleEndian<std::uint64_t>(
            input, PublicationLayout::optionalFeatures),
        orderedRoot,
        blobRoot,
        allocatorRoot};
}

inline void validateCanonicalBootstrap(const Bootstrap& bootstrap) {
    const ByteView input{bootstrap};
    const auto suite = visibleEncryptionSuite(bootstrap);
    if (readLittleEndian<std::uint16_t>(input, BootstrapLayout::reserved16) != 0 ||
        readLittleEndian<std::uint32_t>(input, BootstrapLayout::reserved32) != 0 ||
        readLittleEndian<std::uint64_t>(
            input, BootstrapLayout::commonRegionLength) != commonRegionBytes ||
        readLittleEndian<std::uint32_t>(input, BootstrapLayout::slotLength) !=
            publicationSlotBytes ||
        readLittleEndian<std::uint32_t>(input, BootstrapLayout::slotCount) != 2 ||
        readLittleEndian<std::uint64_t>(input, BootstrapLayout::backendDataOffset) !=
            commonRegionBytes ||
        !allZero(input, BootstrapLayout::reserved, bootstrapBytes) ||
        (suite == EncryptionSuite::None &&
         !allZero(
             input,
             BootstrapLayout::salt,
             BootstrapLayout::commonRegionLength))) {
        throwCorrupt("authenticated bootstrap is noncanonical");
    }
}

[[nodiscard]] inline bool equalPublicationSemantics(
    PublicationPlaintext left,
    PublicationPlaintext right) {
    left[PublicationLayout::slotIndex] =
        left[PublicationLayout::slotIndex + 1] = std::byte{0};
    right[PublicationLayout::slotIndex] =
        right[PublicationLayout::slotIndex + 1] = std::byte{0};
    return left == right;
}

template<class Limits>
[[nodiscard]] inline Result<SelectedPublication, AuthenticationFailed>
selectPublication(
    DurableFile& file,
    const SessionKeys* keys,
    ProviderSet& providers,
    const Bootstrap* alreadyReadBootstrap = nullptr) {
    Bootstrap bootstrap = alreadyReadBootstrap
        ? *alreadyReadBootstrap
        : Bootstrap{};
    if (!alreadyReadBootstrap) {
        file.readExactAt(0, bootstrap);
    }
    validateVisibleBootstrapDispatch(bootstrap);
    const auto encryptionSuite = visibleEncryptionSuite(bootstrap);
    CryptoProvider* crypto = encryptionSuite == EncryptionSuite::None
        ? nullptr
        : &ProviderAccess::crypto(providers);

    std::array<PublicationSlot, 2> slots{};
    std::array<std::optional<PublicationPlaintext>, 2> plaintexts{};
    const auto physicalBytes = file.size();
    for (std::uint16_t index = 0; index != 2; ++index) {
        const auto slotEnd = bootstrapBytes + (index + 1) * publicationSlotBytes;
        if (physicalBytes >= slotEnd) {
            file.readExactAt(
                bootstrapBytes + index * publicationSlotBytes, slots[index]);
            plaintexts[index] = encryptionSuite == EncryptionSuite::None
                ? checksumSlot(slots[index], index, bootstrap)
                : authenticateSlot(
                      slots[index], index, bootstrap, *crypto, keys->header);
        }
    }
    if (!plaintexts[0] && !plaintexts[1]) {
        return Result<SelectedPublication, AuthenticationFailed>::failure(
            AuthenticationFailed{});
    }
    validateCanonicalBootstrap(bootstrap);

    std::array<std::optional<OpenedFormat>, 2> formats{};
    for (std::uint16_t index = 0; index != 2; ++index) {
        if (plaintexts[index]) {
            formats[index] = validatePublication<Limits>(
                *plaintexts[index], index, bootstrap, crypto, providers);
        }
    }

    std::size_t selected = formats[1] &&
            (!formats[0] || formats[1]->generation > formats[0]->generation)
        ? 1
        : 0;
    if (formats[0] && formats[1]) {
        if (formats[0]->generation == formats[1]->generation) {
            if (!equalPublicationSemantics(*plaintexts[0], *plaintexts[1])) {
                throwCorrupt("equal publication generations contradict");
            }
        } else {
            const auto newer = selected;
            const auto older = 1U - selected;
            if (formats[newer]->generation != formats[older]->generation + 1 ||
                readLittleEndian<std::uint64_t>(
                    *plaintexts[newer], PublicationLayout::predecessorGeneration) !=
                    formats[older]->generation) {
                throwCorrupt("publication generations contradict");
            }
        }
    }
    if (physicalBytes < formats[selected]->highWaterBytes) {
        throwCorrupt("database file ends before its committed boundary");
    }
    return Result<SelectedPublication, AuthenticationFailed>::success(
        SelectedPublication{
            *formats[selected],
            bootstrap,
            *plaintexts[selected],
            !formats[1U - selected].has_value(),
            physicalBytes - formats[selected]->highWaterBytes});
}

template<class Limits>
[[nodiscard]] inline Result<OpenedDatabase, AuthenticationFailed> openFormat(
    DurableFile& file,
    EncryptionKeyView callerKey,
    ProviderSet& providers) {
    Bootstrap bootstrap{};
    file.readExactAt(0, bootstrap);
    validateVisibleBootstrapDispatch(bootstrap);
    if (visibleEncryptionSuite(bootstrap) == EncryptionSuite::None) {
        throw DatabaseError{
            Errc::UnexpectedKey,
            "unencrypted database requires a keyless entry point"};
    }
    requireCallerKey(callerKey);

    auto& crypto = ProviderAccess::crypto(providers);
    auto keys = deriveSessionKeys(
        crypto,
        callerKey,
        ByteView{bootstrap}.subspan(
            BootstrapLayout::databaseIdentity, databaseIdentityBytes),
        ByteView{bootstrap}.subspan(BootstrapLayout::salt, kdfSaltBytes));
    auto selected = selectPublication<Limits>(
        file, &keys, providers, &bootstrap);
    if (!selected) {
        return Result<OpenedDatabase, AuthenticationFailed>::failure(
            AuthenticationFailed{});
    }
    auto publication = std::move(selected).value();
    return Result<OpenedDatabase, AuthenticationFailed>::success(
        OpenedDatabase{
            publication.format,
            std::move(keys),
            publication.bootstrap,
            publication.publication,
            publication.rejectedInactivePublication,
            publication.abandonedTailBytes});
}

template<class Limits>
[[nodiscard]] inline OpenedDatabase openUnencryptedFormat(
    DurableFile& file,
    ProviderSet& providers) {
    Bootstrap bootstrap{};
    file.readExactAt(0, bootstrap);
    validateVisibleBootstrapDispatch(bootstrap);
    if (visibleEncryptionSuite(bootstrap) != EncryptionSuite::None) {
        throw DatabaseError{
            Errc::KeyRequired,
            "encrypted database requires a keyed entry point"};
    }
    auto selected = selectPublication<Limits>(
        file, nullptr, providers, &bootstrap);
    if (!selected) {
        throwCorrupt("no unencrypted publication checksum is valid");
    }
    auto publication = std::move(selected).value();
    return OpenedDatabase{
        publication.format,
        std::nullopt,
        publication.bootstrap,
        publication.publication,
        publication.rejectedInactivePublication,
        publication.abandonedTailBytes};
}

} // namespace miare::detail
