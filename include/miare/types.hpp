#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

namespace miare {

using ByteView = std::span<const std::byte>;
using MutableByteView = std::span<std::byte>;

enum class StorageBackend : std::uint32_t {
    BTree,
};

enum class Compression : std::uint32_t {
    None,
    ZStd,
};

enum class EncryptionSuite : std::uint32_t {
    XChaCha20Poly1305Ietf,
};

struct CreateOptions {
    StorageBackend storageBackend = StorageBackend::BTree;
    Compression compression = Compression::ZStd;
    EncryptionSuite encryptionSuite = EncryptionSuite::XChaCha20Poly1305Ietf;
};

struct OpenOptions {
    std::size_t cacheCapacityBytes = 64U * 1024U * 1024U;
    std::uint32_t maxReaders = 256;
};

class EncryptionKeyView {
public:
    EncryptionKeyView() = delete;
    explicit EncryptionKeyView(ByteView bytes) noexcept : bytes_(bytes) {}

    template<std::size_t Size>
    explicit EncryptionKeyView(const std::byte (&bytes)[Size]) noexcept
        : bytes_(bytes) {}

    template<class T, std::size_t Size>
    explicit EncryptionKeyView(const T (&bytes)[Size]) noexcept
        requires(sizeof(T) == 1)
        : bytes_(reinterpret_cast<const std::byte*>(bytes), Size) {}

    template<std::size_t Size>
    explicit EncryptionKeyView(const std::array<std::byte, Size>& bytes) noexcept
        : bytes_(bytes) {}

    [[nodiscard]] ByteView bytes() const noexcept { return bytes_; }

private:
    ByteView bytes_;
};

class BlobId {
public:
    static constexpr std::size_t encodedSize = 16;

    [[nodiscard]] static BlobId fromBytes(
        std::span<const std::byte, encodedSize> bytes) noexcept {
        std::array<std::byte, encodedSize> owned{};
        std::copy(bytes.begin(), bytes.end(), owned.begin());
        return BlobId{owned};
    }

    [[nodiscard]] std::array<std::byte, encodedSize> toBytes() const noexcept {
        return bytes_;
    }

    friend bool operator==(BlobId, BlobId) noexcept = default;
    friend std::strong_ordering operator<=>(BlobId, BlobId) noexcept = default;

private:
    explicit BlobId(std::array<std::byte, encodedSize> bytes) noexcept
        : bytes_(bytes) {}

    std::array<std::byte, encodedSize> bytes_;
};

struct DefaultLimits {
    static constexpr std::uint64_t allocationQuantumBytes = 4ULL * 1024ULL;
    static constexpr std::uint64_t maxInlineValueBytes = 1ULL * 1024ULL;
    static constexpr std::uint64_t blobChunkBytes = 1ULL << 20;
    static constexpr std::uint64_t maxKeyBytes = 4ULL * 1024ULL;
    static constexpr std::uint64_t maxValueBytes = 16ULL * 1024ULL * 1024ULL;
    static constexpr std::uint64_t maxBlobBytes = 1ULL << 40;
    static constexpr std::uint64_t maxDatabaseBytes = 16ULL << 40;
    static constexpr std::uint64_t maxKeyMutationsPerTransaction = 1'000'000;
    static constexpr std::uint64_t maxBlobMutationsPerTransaction = 1'024;
    static constexpr std::uint64_t maxBlobBytesPerTransaction = 1ULL << 40;
    static constexpr std::uint64_t maxFileGrowthPerTransaction = 2ULL << 40;
    static constexpr std::uint32_t maxCursorsPerTransaction = 1'024;
    static constexpr std::uint32_t maxBlobReadersPerTransaction = 1'024;
    static constexpr std::uint32_t maxOpenBlobWritersPerTransaction = 1'024;
};

template<class Allocator>
concept DatabaseAllocator =
    std::same_as<typename std::allocator_traits<Allocator>::value_type, std::byte> &&
    std::copy_constructible<Allocator>;

template<class Limits>
concept LimitPolicy = requires {
    { Limits::allocationQuantumBytes } -> std::convertible_to<std::uint64_t>;
    { Limits::maxInlineValueBytes } -> std::convertible_to<std::uint64_t>;
    { Limits::blobChunkBytes } -> std::convertible_to<std::uint64_t>;
    { Limits::maxKeyBytes } -> std::convertible_to<std::uint64_t>;
    { Limits::maxValueBytes } -> std::convertible_to<std::uint64_t>;
    { Limits::maxBlobBytes } -> std::convertible_to<std::uint64_t>;
    { Limits::maxDatabaseBytes } -> std::convertible_to<std::uint64_t>;
    { Limits::maxKeyMutationsPerTransaction } -> std::convertible_to<std::uint64_t>;
    { Limits::maxBlobMutationsPerTransaction } -> std::convertible_to<std::uint64_t>;
    { Limits::maxBlobBytesPerTransaction } -> std::convertible_to<std::uint64_t>;
    { Limits::maxFileGrowthPerTransaction } -> std::convertible_to<std::uint64_t>;
    { Limits::maxCursorsPerTransaction } -> std::convertible_to<std::uint32_t>;
    { Limits::maxBlobReadersPerTransaction } -> std::convertible_to<std::uint32_t>;
    { Limits::maxOpenBlobWritersPerTransaction } -> std::convertible_to<std::uint32_t>;
} &&
    std::has_single_bit(Limits::allocationQuantumBytes) &&
    Limits::allocationQuantumBytes >= 512 &&
    Limits::allocationQuantumBytes <= 64U * 1024U &&
    std::has_single_bit(Limits::blobChunkBytes) &&
    Limits::blobChunkBytes >= 64U * 1024U &&
    Limits::blobChunkBytes <= 16U * 1024U * 1024U &&
    Limits::blobChunkBytes >= Limits::allocationQuantumBytes &&
    Limits::maxInlineValueBytes <= Limits::maxValueBytes;

template<
    class Allocator = std::allocator<std::byte>,
    class Limits = DefaultLimits>
requires DatabaseAllocator<Allocator> && LimitPolicy<Limits>
class Database {
public:
    using OwnedBytes = std::vector<
        std::byte,
        typename std::allocator_traits<Allocator>::template rebind_alloc<std::byte>>;

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&&) noexcept = default;
    Database& operator=(Database&&) = delete;
    ~Database() = default;

private:
    Database() = default;
};

} // namespace miare

template<>
struct std::hash<miare::BlobId> {
    std::size_t operator()(miare::BlobId id) const noexcept {
        const auto bytes = id.toBytes();
        std::size_t value = 1469598103934665603ULL;
        for (const auto byte : bytes) {
            value ^= std::to_integer<unsigned char>(byte);
            value *= 1099511628211ULL;
        }
        return value;
    }
};
