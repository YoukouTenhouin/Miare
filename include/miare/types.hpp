#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

namespace miare {

/// A borrowed, immutable sequence of bytes.
using ByteView = std::span<const std::byte>;
/// A borrowed, mutable sequence of bytes.
using MutableByteView = std::span<std::byte>;

/// Selects the persistent storage strategy at database creation.
enum class StorageBackend : std::uint32_t {
    /// The V1 copy-on-write ordered B+ tree backend.
    BTree,
};

/// Selects whether backend units may be compressed.
enum class Compression : std::uint32_t {
    /// Store authenticated units without compression.
    None,
    /// Use the frozen V1 Zstandard profile where eligible.
    ZStd,
};

/// Selects the persistent storage-protection construction.
enum class EncryptionSuite : std::uint32_t {
    /// Plaintext protected by the frozen V1 unkeyed checksum profile.
    None,
    /// XChaCha20-Poly1305-IETF with the V1 key derivation profile.
    XChaCha20Poly1305Ietf,
};

/// Describes the lifecycle state of a database handle.
enum class DatabaseState : std::uint8_t {
    /// The session accepts database operations.
    Open,
    /// Close admission is in progress.
    Closing,
    /// Access is restricted until close and reopen recovery.
    RecoveryRequired,
    /// The handle is closed, destroyed, or moved from.
    Closed,
};

/// Identifies why an open session requires recovery.
enum class RecoveryCause : std::uint8_t {
    /// The session does not require recovery.
    None = 0,
    /// A failed commit is known not to have published its candidate.
    CommitKnownUnpublished,
    /// Reopen must determine whether a failed commit published.
    CommitOutcomeUnknown,
    /// Maintenance failed after persistent source mutation began.
    MaintenancePersistenceFailed,
    /// Close failed after consolidation or durability work began.
    ClosePersistenceFailed,
    /// Authenticated state was positively established as corrupt.
    ConfirmedCorruption,
};

/// Classifies the effect of an integrity-verification finding.
enum class VerificationSeverity : std::uint8_t {
    /// The authoritative database state violates an integrity invariant.
    Corruption = 0,
    /// A physical condition is noteworthy but does not invalidate the state.
    Observation = 1,
};

/// Identifies the structural role of an extent in a verification finding.
enum class VerificationExtentRole : std::uint8_t {
    /// No more specific role is available.
    Unknown = 0,
    /// An internal ordered-keyspace B+ tree page.
    OrderedInternal,
    /// An ordered-keyspace B+ tree leaf page.
    OrderedLeaf,
    /// An extent containing an overflow value.
    OverflowValue,
    /// An internal Blob-catalog page.
    BlobCatalogInternal,
    /// A Blob-catalog leaf page.
    BlobCatalogLeaf,
    /// A Blob version manifest.
    BlobManifest,
    /// An internal Blob chunk-index page.
    BlobChunkIndexInternal,
    /// A Blob chunk-index leaf page.
    BlobChunkIndexLeaf,
    /// A Blob content chunk.
    BlobChunk,
    /// The persistent allocator root.
    AllocatorRoot,
    /// An internal free-run index page.
    FreeIndexInternal,
    /// A free-run index leaf page.
    FreeIndexLeaf,
    /// An internal retired-run index page.
    RetiredIndexInternal,
    /// A retired-run index leaf page.
    RetiredIndexLeaf,
};

/// Stable machine-readable codes emitted by integrity verification.
enum class VerificationFindingCode : std::uint16_t {
    /// An inactive publication was incomplete and rejected.
    IncompleteInactivePublication = 1,
    /// Physical bytes follow the selected committed boundary.
    AbandonedTail = 2,
    /// Publication metadata cannot be selected consistently.
    PublicationConflict = 0x100,
    /// The physical file is shorter than authoritative bounds require.
    FileTruncated,
    /// An extent reference lies outside valid file bounds.
    ExtentOutOfBounds,
    /// An extent's self-framing is invalid.
    ExtentFramingInvalid,
    /// An extent failed authenticated decryption.
    ExtentAuthenticationFailed,
    /// A compressed or uncompressed representation envelope is invalid.
    CodecEnvelopeInvalid,
    /// A bounded representation could not be decoded.
    DecodeFailed,
    /// Decoded structural bytes are not canonical.
    CanonicalEncodingInvalid,
    /// A child or owner reference disagrees with authenticated metadata.
    ReferenceMismatch,
    /// An extent appears in a structural role other than the expected role.
    RoleMismatch,
    /// An extent generation violates the reachable graph.
    GenerationMismatch,
    /// A persistent tree violates topology invariants.
    TreeTopologyInvalid,
    /// A persistent tree violates key-ordering invariants.
    TreeOrderingInvalid,
    /// A Blob manifest, index, or chunk relationship is invalid.
    BlobInvariantInvalid,
    /// Allocator classifications overlap.
    AllocationOverlap,
    /// The authoritative high-water partition has an unclassified gap.
    AllocationGap,
    /// Authenticated allocation counters disagree with the partition.
    AllocationCountMismatch,
    /// One physical extent is reachable through incompatible paths.
    DuplicateReachability,
    /// An unencrypted extent failed its suite-0 checksum.
    ExtentChecksumFailed,
};

/// One bounded, content-free integrity-verification finding.
struct VerificationFinding {
    /// Whether this finding invalidates the authoritative state.
    VerificationSeverity severity = VerificationSeverity::Corruption;
    /// Stable machine-readable finding category.
    VerificationFindingCode code =
        VerificationFindingCode::CanonicalEncodingInvalid;
    /// First allocation block involved, or `uint64_t::max()` when unavailable.
    std::uint64_t blockIndex = std::numeric_limits<std::uint64_t>::max();
    /// Number of allocation blocks involved.
    std::uint64_t blockCount = 0;
    /// Structural role associated with the finding.
    VerificationExtentRole extentRole = VerificationExtentRole::Unknown;
    /// Related generation, or `uint64_t::max()` when unavailable.
    std::uint64_t generation = std::numeric_limits<std::uint64_t>::max();
};

/// Result of full integrity verification using an allocator-aware finding list.
template<class Allocator = std::allocator<std::byte>>
struct BasicVerificationReport {
    /// Allocator used by the finding list.
    using FindingAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<VerificationFinding>;

    /// Constructs an empty valid report using `allocator`.
    explicit BasicVerificationReport(const Allocator& allocator = {})
        : findings(FindingAllocator{allocator}) {}

    /// `false` when any corruption-severity finding was discovered.
    bool valid = true;
    /// Authoritative generation selected for verification.
    std::uint64_t selectedGeneration = 0;
    /// Number of authenticated extents examined.
    std::uint64_t extentsChecked = 0;
    /// Total encoded extent bytes examined.
    std::uint64_t encodedBytesChecked = 0;
    /// Total decoded structural or content bytes examined.
    std::uint64_t decodedBytesChecked = 0;
    /// Ordered-keyspace entries examined.
    std::uint64_t keysChecked = 0;
    /// Blob versions examined.
    std::uint64_t blobsChecked = 0;
    /// Blob content chunks examined.
    std::uint64_t blobChunksChecked = 0;
    /// Allocation blocks classified as reachable live state.
    std::uint64_t liveBlocks = 0;
    /// Allocation blocks classified as free.
    std::uint64_t freeBlocks = 0;
    /// Allocation blocks classified as generation-retired.
    std::uint64_t retiredBlocks = 0;
    /// Physical blocks beyond the selected committed boundary.
    std::uint64_t abandonedTailBlocks = 0;
    /// Bounded structural findings without application content.
    std::vector<VerificationFinding, FindingAllocator> findings;
    /// Whether additional findings were omitted after reaching the bound.
    bool findingsTruncated = false;
};

/// Default-allocator integrity-verification report.
using VerificationReport = BasicVerificationReport<>;

/// Physical summary returned by a successful portable backup.
struct BackupReport {
    /// Committed generation copied to the destination.
    std::uint64_t sourceGeneration = 0;
    /// Installed destination file size.
    std::uint64_t destinationFileBytes = 0;
    /// Source extents authenticated before copying.
    std::uint64_t extentsVerified = 0;
    /// Encoded source bytes authenticated before copying.
    std::uint64_t encodedBytesVerified = 0;
    /// Live blocks in the selected source partition.
    std::uint64_t liveBlocks = 0;
    /// Free blocks preserved in the physical snapshot.
    std::uint64_t freeBlocks = 0;
    /// Retired blocks preserved in the physical snapshot.
    std::uint64_t retiredBlocks = 0;
    /// Whether open had rejected an incomplete inactive publication.
    bool hadIncompleteInactivePublication = false;
    /// Abandoned-tail bytes deliberately excluded from the backup.
    std::uint64_t excludedAbandonedTailBytes = 0;
};

/// Persistent choices used only when creating a database.
struct CreateOptions {
    /// Storage backend fixed for the database lifetime.
    StorageBackend storageBackend = StorageBackend::BTree;
    /// Persistent compression policy.
    Compression compression = Compression::ZStd;
    /// Persistent authenticated-encryption suite.
    EncryptionSuite encryptionSuite = EncryptionSuite::XChaCha20Poly1305Ietf;
};

/// Persistent choices used only when creating an unencrypted database.
struct UnencryptedCreateOptions {
    /// Storage backend fixed for the database lifetime.
    StorageBackend storageBackend = StorageBackend::BTree;
    /// Persistent compression policy.
    Compression compression = Compression::None;
};

/// Runtime budgets applied when opening a database.
struct OpenOptions {
    /// Budget for retained decoded pages and Blob chunks.
    std::size_t cacheCapacityBytes = 64U * 1024U * 1024U;
    /// Maximum concurrently active read transactions.
    std::uint32_t maxReaders = 256;
};

/// Current capacity accounting for an active write transaction.
struct WriteTransactionStats {
    /// Successful ordered-keyspace mutations.
    std::uint64_t keyMutations = 0;
    /// Successful Blob mutations currently charged to the transaction.
    std::uint64_t blobMutations = 0;
    /// Blob bytes accepted by writers in this transaction.
    std::uint64_t blobBytesWritten = 0;
    /// Conservative estimate of additional physical storage required.
    std::uint64_t estimatedFileGrowthBytes = 0;
    /// Blob writers that must be finished or aborted before commit.
    std::uint32_t openBlobWriters = 0;
};

/// Content-free point-in-time operational state for an open session.
struct DiagnosticsSnapshot {
    /// Current database lifecycle state.
    DatabaseState state = DatabaseState::Closed;
    /// Selected portable-format version.
    std::uint32_t formatVersion = 0;
    /// Capacity-profile identity version.
    std::uint32_t capacityProfileVersion = 0;
    /// Canonical digest of every compile-time capacity value.
    std::array<std::byte, 32> capacityProfileDigest{};
    /// Selected persistent storage backend.
    StorageBackend storageBackend = StorageBackend::BTree;
    /// Selected persistent compression policy.
    Compression compression = Compression::None;
    /// Selected persistent encryption suite.
    EncryptionSuite encryptionSuite = EncryptionSuite::None;
    /// Last generation known to be committed.
    std::uint64_t lastCommittedGeneration = 0;
    /// Current main database file size.
    std::uint64_t mainFileBytes = 0;
    /// Current data-bearing sidecar size.
    std::uint64_t sidecarBytes = 0;
    /// Estimated bytes reachable from current committed state.
    std::uint64_t liveBytes = 0;
    /// Estimated bytes eligible for reclamation.
    std::uint64_t reclaimableBytes = 0;
    /// Estimated bytes retained solely for live snapshots.
    std::uint64_t snapshotRetainedBytes = 0;
    /// Configured decoded-cache budget.
    std::uint64_t cacheCapacityBytes = 0;
    /// Decoded-cache bytes currently retained.
    std::uint64_t cacheUsedBytes = 0;
    /// Decoded-cache bytes pinned by active operations.
    std::uint64_t cachePinnedBytes = 0;
    /// Number of decoded-cache evictions.
    std::uint64_t cacheEvictions = 0;
    /// Active read transactions.
    std::uint32_t activeReaders = 0;
    /// Oldest live snapshot generation, if a reader exists.
    std::optional<std::uint64_t> oldestReaderGeneration;
    /// Age of the oldest live snapshot.
    std::chrono::milliseconds oldestReaderAge{0};
    /// Whether a write transaction currently owns the writer lane.
    bool writerActive = false;
    /// Whether maintenance currently owns its admission lane.
    bool maintenanceActive = false;
    /// Number of operations queued for FIFO writer-lane admission.
    std::uint32_t writerQueueDepth = 0;
    /// Whether ordinary access is blocked until recovery.
    bool recoveryRequired = false;
    /// Stable reason ordinary access is blocked.
    RecoveryCause recoveryCause = RecoveryCause::None;
    /// Whether open rejected an incomplete inactive publication.
    bool rejectedInactivePublication = false;
    /// Physical bytes beyond the selected committed boundary.
    std::uint64_t abandonedTailBytes = 0;
    /// Number of resource or capacity failures observed by the session.
    std::uint64_t capacityFailureCount = 0;
    /// Blob-owned subset of `reclaimableBytes`.
    std::uint64_t blobReclaimableBytes = 0;
    /// Blob-owned subset of `snapshotRetainedBytes`.
    std::uint64_t blobSnapshotRetainedBytes = 0;
};

/// Non-owning view of caller-supplied high-entropy encryption key material.
class EncryptionKeyView {
public:
    EncryptionKeyView() = delete;
    /// Borrows the bytes in `bytes`; the selected suite validates their length.
    explicit EncryptionKeyView(ByteView bytes) noexcept : bytes_(bytes) {}

    /// Borrows a fixed-size byte array.
    template<std::size_t Size>
    explicit EncryptionKeyView(const std::byte (&bytes)[Size]) noexcept
        : bytes_(bytes) {}

    /// Borrows a fixed-size array of one-byte elements.
    template<class T, std::size_t Size>
    explicit EncryptionKeyView(const T (&bytes)[Size]) noexcept
        requires(sizeof(T) == 1)
        : bytes_(reinterpret_cast<const std::byte*>(bytes), Size) {}

    /// Borrows a fixed-size `std::array` of bytes.
    template<std::size_t Size>
    explicit EncryptionKeyView(const std::array<std::byte, Size>& bytes) noexcept
        : bytes_(bytes) {}

    template<std::size_t Size>
    EncryptionKeyView(std::array<std::byte, Size>&&) = delete;

    template<std::size_t Size>
    EncryptionKeyView(const std::array<std::byte, Size>&&) = delete;

    /// Returns the borrowed key bytes.
    [[nodiscard]] ByteView bytes() const noexcept { return bytes_; }

private:
    ByteView bytes_;
};

/// Opaque, stable identity of a Blob within one database.
class BlobId {
public:
    /// Size of the canonical byte representation.
    static constexpr std::size_t encodedSize = 16;

    /// Decodes a Blob identifier from its canonical representation.
    [[nodiscard]] static BlobId fromBytes(
        std::span<const std::byte, encodedSize> bytes) noexcept {
        std::array<std::byte, encodedSize> owned{};
        std::copy(bytes.begin(), bytes.end(), owned.begin());
        return BlobId{owned};
    }

    /// Returns the canonical byte representation.
    [[nodiscard]] std::array<std::byte, encodedSize> toBytes() const noexcept {
        return bytes_;
    }

    /// Compares canonical identifier bytes for equality.
    friend bool operator==(BlobId, BlobId) noexcept = default;
    /// Orders canonical identifier bytes lexicographically.
    friend std::strong_ordering operator<=>(BlobId, BlobId) noexcept = default;

private:
    explicit BlobId(std::array<std::byte, encodedSize> bytes) noexcept
        : bytes_(bytes) {}

    std::array<std::byte, encodedSize> bytes_;
};

/// Performance-qualified default persistent capacity profile.
struct DefaultLimits {
    /// Smallest addressable span of backend-owned storage.
    static constexpr std::uint64_t allocationQuantumBytes = 4ULL * 1024ULL;
    /// Largest value kept inside an ordered leaf entry.
    static constexpr std::uint64_t maxInlineValueBytes = 1ULL * 1024ULL;
    /// Logical bytes in each non-final Blob chunk.
    static constexpr std::uint64_t blobChunkBytes = 1ULL << 20;
    /// Maximum key length.
    static constexpr std::uint64_t maxKeyBytes = 4ULL * 1024ULL;
    /// Maximum ordinary value length.
    static constexpr std::uint64_t maxValueBytes = 16ULL * 1024ULL * 1024ULL;
    /// Maximum logical Blob length.
    static constexpr std::uint64_t maxBlobBytes = 1ULL << 40;
    /// Maximum committed main-file envelope.
    static constexpr std::uint64_t maxDatabaseBytes = 16ULL << 40;
    /// Maximum successful key mutations in one write transaction.
    static constexpr std::uint64_t maxKeyMutationsPerTransaction = 1'000'000;
    /// Maximum Blob mutations in one write transaction.
    static constexpr std::uint64_t maxBlobMutationsPerTransaction = 1'024;
    /// Maximum Blob bytes accepted in one write transaction.
    static constexpr std::uint64_t maxBlobBytesPerTransaction = 1ULL << 40;
    /// Maximum estimated aggregate file growth for one transaction.
    static constexpr std::uint64_t maxFileGrowthPerTransaction = 2ULL << 40;
    /// Maximum live cursors belonging to one transaction.
    static constexpr std::uint32_t maxCursorsPerTransaction = 1'024;
    /// Maximum live Blob readers belonging to one transaction.
    static constexpr std::uint32_t maxBlobReadersPerTransaction = 1'024;
    /// Maximum unfinished Blob writers in one write transaction.
    static constexpr std::uint32_t maxOpenBlobWritersPerTransaction = 1'024;
};

/// Requirements for an allocator used by `Database`.
template<class Allocator>
concept DatabaseAllocator =
    std::same_as<typename std::allocator_traits<Allocator>::value_type, std::byte> &&
    std::copy_constructible<Allocator>;

/// Compile-time structural requirements for a persistent capacity profile.
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
