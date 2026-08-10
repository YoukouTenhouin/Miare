#include <miare/database.hpp>
#include <miare/testing/fakes.hpp>

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

void requireTest(
    bool condition,
    const std::source_location location = std::source_location::current()) {
    if (!condition) {
        std::cerr << "test requirement failed at " << location.file_name() << ':'
                  << location.line() << '\n';
        std::cerr.flush();
        std::abort();
    }
}

template<class Operation>
void runTestCase(const char* name, Operation&& operation) {
    std::cerr << "running exact-transaction case: " << name << '\n';
    std::cerr.flush();
    operation();
}

template<class Predicate>
[[nodiscard]] bool waitUntil(Predicate&& predicate) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

struct AllocationCounts {
    std::atomic<std::size_t> allocatedBytes{0};
    std::atomic<std::size_t> deallocatedBytes{0};
    std::atomic<std::size_t> largestAllocationBytes{0};
};

template<class T>
class CountingAllocator {
public:
    using value_type = T;

    CountingAllocator() : counts(std::make_shared<AllocationCounts>()) {}
    explicit CountingAllocator(std::shared_ptr<AllocationCounts> sharedCounts)
        : counts(std::move(sharedCounts)) {}

    CountingAllocator(const CountingAllocator&) noexcept = default;
    // A moved-from allocator must remain usable by moved-from containers.
    CountingAllocator(CountingAllocator&& other) noexcept
        : counts(other.counts) {}

    template<class U>
    CountingAllocator(const CountingAllocator<U>& other) noexcept
        : counts(other.counts) {}

    [[nodiscard]] T* allocate(std::size_t count) {
        const auto bytes = count * sizeof(T);
        counts->allocatedBytes.fetch_add(bytes, std::memory_order_relaxed);
        auto largest = counts->largestAllocationBytes.load(std::memory_order_relaxed);
        while (largest < bytes &&
               !counts->largestAllocationBytes.compare_exchange_weak(
                   largest, bytes, std::memory_order_relaxed)) {
        }
        return std::allocator<T>{}.allocate(count);
    }

    void deallocate(T* allocation, std::size_t count) noexcept {
        counts->deallocatedBytes.fetch_add(
            count * sizeof(T), std::memory_order_relaxed);
        std::allocator<T>{}.deallocate(allocation, count);
    }

    [[nodiscard]] CountingAllocator select_on_container_copy_construction() const {
        return CountingAllocator{};
    }

    template<class U>
    friend class CountingAllocator;

    template<class U>
    friend bool operator==(
        const CountingAllocator& left,
        const CountingAllocator<U>& right) noexcept {
        return left.counts == right.counts;
    }

    std::shared_ptr<AllocationCounts> counts;
};

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto suffix = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        path_ = std::filesystem::temp_directory_path() /
            ("miare-exact-transaction-" + suffix);
        std::filesystem::create_directory(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

constexpr std::array<std::byte, 32> keyBytes{};

using DefaultRead = miare::Database<>::ReadTransaction;
using DefaultWrite = miare::Database<>::WriteTransaction;
static_assert(std::is_move_constructible_v<DefaultRead>);
static_assert(!std::is_copy_constructible_v<DefaultRead>);
static_assert(!std::is_move_assignable_v<DefaultRead>);
static_assert(std::is_move_constructible_v<DefaultWrite>);
static_assert(!std::is_copy_constructible_v<DefaultWrite>);
static_assert(!std::is_move_assignable_v<DefaultWrite>);

[[nodiscard]] miare::ByteView bytes(const char* text) {
    return {
        reinterpret_cast<const std::byte*>(text),
        std::char_traits<char>::length(text)};
}

[[nodiscard]] bool equals(
    const std::optional<miare::Database<>::OwnedBytes>& actual,
    const char* expected) {
    const auto expectedBytes = bytes(expected);
    return actual && std::equal(
        actual->begin(), actual->end(), expectedBytes.begin(), expectedBytes.end());
}

[[nodiscard]] miare::ProviderSet deterministicProviders(std::uint64_t seed) {
    return miare::detail::ProviderAccess::make(
        std::make_unique<miare::testing::DeterministicCryptoProvider>(seed),
        std::make_unique<miare::testing::FaultInjectingCompressionProvider>());
}

[[nodiscard]] bool imageContains(
    const std::vector<std::byte>& image,
    const char* key,
    const char* value);

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

void exactMutationsAreAtomicAndDurable(const TemporaryDirectory& temporary) {
    const auto path = temporary.path() / "exact.miare";
    auto database = miare::Database<>::create(
        path,
        miare::EncryptionKeyView{keyBytes},
        deterministicProviders(1));

    auto empty = database.beginRead();
    assert(!empty.get(bytes("missing")));
    assert(!empty.contains(bytes("missing")));

    auto write = database.beginWrite();
    write.put(bytes("alpha"), bytes("one"));
    write.put(bytes("\x80"), bytes("high"));
    write.put(bytes("\x7f"), bytes("low"));
    assert(equals(write.get(bytes("alpha")), "one"));
    assert(write.contains(bytes("\x80")));
    write.put(bytes("alpha"), bytes("two"));
    assert(equals(write.get(bytes("alpha")), "two"));
    assert(write.erase(bytes("\x7f")));
    assert(!write.erase(bytes("\x7f")));
    assert(!write.get(bytes("\x7f")));
    assert(write.stats().keyMutations == 5);
    assert(write.stats().estimatedFileGrowthBytes == 16U * 1024U);
    write.commit();

    assert(!empty.contains(bytes("alpha")));
    empty.end();
    auto committed = database.beginRead();
    assert(equals(committed.get(bytes("alpha")), "two"));
    assert(equals(committed.get(bytes("\x80")), "high"));
    committed.end();

    auto secondWrite = database.beginWrite();
    assert(secondWrite.erase(bytes("alpha")));
    secondWrite.put(bytes("\x80"), bytes("higher"));
    secondWrite.put(bytes(""), bytes(""));
    secondWrite.commit();
    database.close();

    auto reopened = miare::Database<>::open(
        path,
        miare::EncryptionKeyView{keyBytes},
        deterministicProviders(2));
    assert(reopened);
    auto persisted = reopened.value().beginRead();
    assert(!persisted.contains(bytes("alpha")));
    assert(equals(persisted.get(bytes("\x80")), "higher"));
    assert(equals(persisted.get(bytes("")), ""));
    persisted.end();
    reopened.value().close();
}

void rollbackAndValidationPreserveCommittedState(
    const TemporaryDirectory& temporary) {
    const auto path = temporary.path() / "rollback.miare";
    auto database = miare::Database<>::create(
        path,
        miare::EncryptionKeyView{keyBytes},
        deterministicProviders(3));
    {
        auto seed = database.beginWrite();
        seed.put(bytes("stable"), bytes("before"));
        seed.commit();
    }
    {
        auto rolledBack = database.beginWrite();
        rolledBack.put(bytes("stable"), bytes("rolled back"));
        rolledBack.put(bytes("new"), bytes("discarded"));
        rolledBack.rollback();
        assert(!rolledBack.active());
        expectContractError(miare::Errc::InvalidState, [&] {
            (void)rolledBack.get(bytes("stable"));
        });
    }
    {
        auto abandoned = database.beginWrite();
        assert(abandoned.erase(bytes("stable")));
    }

    auto invalid = database.beginWrite();
    std::vector<std::byte> oversizedKey(miare::DefaultLimits::maxKeyBytes + 1);
    expectContractError(miare::Errc::InvalidArgument, [&] {
        invalid.put(oversizedKey, bytes("value"));
    });
    assert(equals(invalid.get(bytes("stable")), "before"));
    std::vector<std::byte> overflowValue(
        miare::DefaultLimits::maxInlineValueBytes + 1,
        std::byte{0x5a});
    invalid.put(bytes("later-overflow"), overflowValue);
    assert(invalid.get(bytes("later-overflow")));
    assert(invalid.active());
    invalid.rollback();

    auto read = database.beginRead();
    assert(equals(read.get(bytes("stable")), "before"));
    assert(!read.contains(bytes("new")));
    read.end();
    database.close();
}

void commitUsesTwoDurabilityBarriersAndFailsStop() {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto* fileView = file.get();
    auto database = miare::testing::DatabaseAccess::create(
        std::move(file),
        miare::EncryptionKeyView{keyBytes},
        deterministicProviders(4));
    fileView->clearOperations();

    auto write = database.beginWrite();
    write.put(bytes("key"), bytes("value"));
    write.commit();
    const auto& operations = fileView->operations();
    assert(operations.size() == 4);
    assert(operations[0].kind == miare::testing::DurableFileOperationKind::Write);
    assert(operations[0].offset == miare::detail::commonRegionBytes);
    assert(operations[1].kind == miare::testing::DurableFileOperationKind::Barrier);
    assert(operations[2].kind == miare::testing::DurableFileOperationKind::Write);
    assert(operations[2].offset == miare::detail::bootstrapBytes);
    assert(operations[3].kind == miare::testing::DurableFileOperationKind::Barrier);
    database.close();

    auto failingFile = std::make_unique<miare::testing::MemoryDurableFile>();
    auto* failingFileView = failingFile.get();
    auto failing = miare::testing::DatabaseAccess::create(
        std::move(failingFile),
        miare::EncryptionKeyView{keyBytes},
        deterministicProviders(5));
    auto oldSnapshot = failing.beginRead();
    auto failedWrite = failing.beginWrite();
    failedWrite.put(bytes("uncommitted"), bytes("value"));
    failingFileView->clearOperations();
    failingFileView->failNextBarrier();
    expectDatabaseError(miare::Errc::CommitFailed, [&] {
        failedWrite.commit();
    });
    assert(!failedWrite.active());
    assert(failing.state() == miare::DatabaseState::RecoveryRequired);
    assert(!oldSnapshot.contains(bytes("uncommitted")));
    expectDatabaseError(miare::Errc::RecoveryRequired, [&] {
        (void)failing.beginRead();
    });
    oldSnapshot.end();
    failing.close();

    auto uncertainFile = std::make_unique<miare::testing::MemoryDurableFile>();
    auto* uncertainFileView = uncertainFile.get();
    auto uncertain = miare::testing::DatabaseAccess::create(
        std::move(uncertainFile),
        miare::EncryptionKeyView{keyBytes},
        deterministicProviders(8));
    auto uncertainWrite = uncertain.beginWrite();
    uncertainWrite.put(bytes("maybe"), bytes("published"));
    uncertainFileView->failBarrierAfter(1);
    expectDatabaseError(miare::Errc::CommitOutcomeUnknown, [&] {
        uncertainWrite.commit();
    });
    assert(!uncertainWrite.active());
    uncertainFileView->simulateCrash();
    const auto recoveredOldImage = uncertainFileView->bytes();
    assert(!imageContains(recoveredOldImage, "maybe", "published"));
    uncertain.close();
}

[[nodiscard]] bool imageContains(
    const std::vector<std::byte>& image,
    const char* key,
    const char* value) {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    file->replaceStableBytes(image);
    auto opened = miare::testing::DatabaseAccess::open(
        std::move(file),
        miare::EncryptionKeyView{keyBytes},
        deterministicProviders(6));
    assert(opened);
    auto read = opened.value().beginRead();
    const bool found = equals(read.get(bytes(key)), value);
    read.end();
    opened.value().close();
    return found;
}

void expectImageDatabaseError(
    const std::vector<std::byte>& image,
    miare::Errc expected) {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    file->replaceStableBytes(image);
    expectDatabaseError(expected, [&] {
        (void)miare::testing::DatabaseAccess::open(
            std::move(file),
            miare::EncryptionKeyView{keyBytes},
            deterministicProviders(16));
    });
}

[[nodiscard]] std::vector<std::byte> rewriteLeafAsCompressed(
    const std::vector<std::byte>& image,
    bool retainUncompressedSpan) {
    constexpr auto quantum = miare::DefaultLimits::allocationQuantumBytes;
    constexpr auto pageCeiling = 16U * 1024U;
    auto providers = deterministicProviders(19);
    miare::testing::MemoryDurableFile file;
    file.replaceStableBytes(image);
    auto openedResult = miare::detail::openFormat<miare::DefaultLimits>(
        file,
        miare::EncryptionKeyView{keyBytes},
        providers);
    assert(openedResult);
    auto opened = std::move(openedResult).value();
    auto reference = miare::detail::decodeExtentReference(
        opened.format.orderedRoot);
    std::vector<std::byte> source(reference.blockCount * quantum);
    file.readExactAt(reference.blockIndex * quantum, source);
    const miare::ByteView sourceView{source};
    const auto sourceStoredLength = miare::detail::readLittleEndian<std::uint64_t>(
        sourceView, miare::detail::ExtentLayout::storedLength);
    const auto decodedLength = miare::detail::readLittleEndian<std::uint64_t>(
        sourceView, miare::detail::ExtentLayout::decodedLength);
    std::vector<std::byte> stored(sourceStoredLength);
    const auto sourceAssociatedData = miare::detail::extentAssociatedData(
        opened,
        sourceView.first(miare::detail::ExtentLayout::bytes));
    auto& crypto = miare::detail::ProviderAccess::crypto(providers);
    const bool decrypted = crypto.decryptDetached(
        opened.keys.mainData.view(),
        sourceView.subspan(
            miare::detail::ExtentLayout::nonce,
            miare::detail::aeadNonceBytes),
        sourceView.subspan(
            miare::detail::ExtentLayout::bytes,
            sourceStoredLength),
        sourceView.subspan(
            miare::detail::ExtentLayout::bytes + sourceStoredLength,
            miare::detail::authenticationTagBytes),
        sourceAssociatedData,
        stored);
    assert(decrypted);
    if (!decrypted) {
        throw miare::DatabaseError{
            miare::Errc::Corrupt,
            "test fixture leaf authentication failed"};
    }

    std::vector<std::byte> decoded(decodedLength);
    if (miare::detail::readLittleEndian<std::uint32_t>(
            sourceView, miare::detail::ExtentLayout::flags) == 1) {
        miare::detail::ProviderAccess::compression(providers).decompress(
            stored, decoded);
    } else {
        decoded = std::move(stored);
    }
    auto& compression = miare::detail::ProviderAccess::compression(providers);
    std::vector<std::byte> compressed(compression.compressBound(decoded.size()));
    compressed.resize(compression.compress(decoded, compressed));
    const auto encodedLength = miare::detail::ExtentLayout::bytes +
        compressed.size() + miare::detail::authenticationTagBytes;
    const auto minimalBlocks =
        encodedLength / quantum + (encodedLength % quantum != 0);
    reference.blockCount = retainUncompressedSpan
        ? pageCeiling / quantum
        : minimalBlocks;
    reference.encodedLength = encodedLength;

    std::vector<std::byte> extent(reference.blockCount * quantum);
    std::copy_n(
        source.begin(),
        miare::detail::ExtentLayout::bytes,
        extent.begin());
    miare::MutableByteView extentOutput{extent};
    miare::detail::writeLittleEndian<std::uint32_t>(
        1, extentOutput, miare::detail::ExtentLayout::flags);
    miare::detail::writeLittleEndian<std::uint32_t>(
        1, extentOutput, miare::detail::ExtentLayout::codec);
    miare::detail::writeLittleEndian<std::uint32_t>(
        1, extentOutput, miare::detail::ExtentLayout::codecProfile);
    miare::detail::writeLittleEndian<std::uint64_t>(
        reference.blockCount,
        extentOutput,
        miare::detail::ExtentLayout::blockCount);
    miare::detail::writeLittleEndian<std::uint64_t>(
        reference.encodedLength,
        extentOutput,
        miare::detail::ExtentLayout::encodedLength);
    miare::detail::writeLittleEndian<std::uint64_t>(
        compressed.size(),
        extentOutput,
        miare::detail::ExtentLayout::storedLength);
    std::array<std::byte, miare::detail::aeadNonceBytes> nonce{};
    crypto.randomBytes(nonce);
    miare::detail::writeBytes(
        extentOutput, miare::detail::ExtentLayout::nonce, nonce);
    const auto associatedData = miare::detail::extentAssociatedData(
        opened,
        miare::ByteView{extent}.first(miare::detail::ExtentLayout::bytes));
    crypto.encryptDetached(
        opened.keys.mainData.view(),
        nonce,
        compressed,
        associatedData,
        miare::MutableByteView{extent}.subspan(
            miare::detail::ExtentLayout::bytes,
            compressed.size()),
        miare::MutableByteView{extent}.subspan(
            miare::detail::ExtentLayout::bytes + compressed.size(),
            miare::detail::authenticationTagBytes));
    const auto highWaterBytes =
        (reference.blockIndex + reference.blockCount) * quantum;
    file.writeExactAt(reference.blockIndex * quantum, extent);
    file.resize(highWaterBytes);

    auto publication = opened.publication;
    const auto encodedReference = miare::detail::encodeExtentReference(reference);
    miare::detail::writeBytes(
        publication,
        miare::detail::PublicationLayout::orderedRoot,
        encodedReference);
    miare::detail::writeLittleEndian<std::uint64_t>(
        highWaterBytes / quantum,
        publication,
        miare::detail::PublicationLayout::highWaterBlocks);
    const auto slotIndex = static_cast<std::uint16_t>(opened.format.generation % 2);
    const auto slot = miare::detail::encodePublicationSlot(
        opened, publication, slotIndex, providers);
    file.writeExactAt(
        miare::detail::bootstrapBytes +
            slotIndex * miare::detail::publicationSlotBytes,
        slot);
    file.stableStorageBarrier();
    return file.bytes();
}

[[nodiscard]] std::vector<std::byte> committedImage(
    miare::Compression compression) {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto* fileView = file.get();
    miare::CreateOptions options;
    options.compression = compression;
    auto database = miare::testing::DatabaseAccess::create(
        std::move(file),
        miare::EncryptionKeyView{keyBytes},
        deterministicProviders(20),
        options);
    auto write = database.beginWrite();
    write.put(bytes("key"), bytes("compressible value"));
    write.commit();
    const auto image = fileView->bytes();
    database.close();
    return image;
}

[[nodiscard]] std::vector<std::byte> generationOneWithBackendData() {
    constexpr auto quantum = miare::DefaultLimits::allocationQuantumBytes;
    auto providers = deterministicProviders(25);
    miare::testing::MemoryDurableFile file;
    const auto commonRegion =
        miare::detail::makeInitialCommonRegion<miare::DefaultLimits>(
            miare::EncryptionKeyView{keyBytes},
            miare::detail::ProviderAccess::crypto(providers),
            miare::Compression::ZStd);
    file.replaceStableBytes(commonRegion);
    auto openedResult = miare::detail::openFormat<miare::DefaultLimits>(
        file,
        miare::EncryptionKeyView{keyBytes},
        providers);
    assert(openedResult);
    auto opened = std::move(openedResult).value();
    file.resize(miare::detail::commonRegionBytes + quantum);
    for (std::uint16_t slotIndex = 0; slotIndex != 2; ++slotIndex) {
        auto publication = opened.publication;
        miare::MutableByteView output{publication};
        miare::detail::writeLittleEndian<std::uint16_t>(
            slotIndex, output, miare::detail::PublicationLayout::slotIndex);
        miare::detail::writeLittleEndian<std::uint64_t>(
            (miare::detail::commonRegionBytes + quantum) / quantum,
            output,
            miare::detail::PublicationLayout::highWaterBlocks);
        const auto slot = miare::detail::encodePublicationSlot(
            opened, publication, slotIndex, providers);
        file.writeExactAt(
            miare::detail::bootstrapBytes +
                slotIndex * miare::detail::publicationSlotBytes,
            slot);
    }
    file.stableStorageBarrier();
    return file.bytes();
}

void generationOneMustBeTheCanonicalEmptyDatabase() {
    expectImageDatabaseError(generationOneWithBackendData(), miare::Errc::Corrupt);
}

[[nodiscard]] std::vector<std::byte> rewriteLeafReferenceSpan(
    const std::vector<std::byte>& image,
    std::uint64_t blockCount) {
    constexpr auto quantum = miare::DefaultLimits::allocationQuantumBytes;
    auto providers = deterministicProviders(21);
    miare::testing::MemoryDurableFile file;
    file.replaceStableBytes(image);
    auto openedResult = miare::detail::openFormat<miare::DefaultLimits>(
        file,
        miare::EncryptionKeyView{keyBytes},
        providers);
    assert(openedResult);
    auto opened = std::move(openedResult).value();
    auto reference = miare::detail::decodeExtentReference(
        opened.format.orderedRoot);
    reference.blockCount = blockCount;
    const auto highWaterBytes =
        (reference.blockIndex + reference.blockCount) * quantum;
    file.resize(highWaterBytes);
    auto publication = opened.publication;
    const auto encodedReference = miare::detail::encodeExtentReference(reference);
    miare::detail::writeBytes(
        publication,
        miare::detail::PublicationLayout::orderedRoot,
        encodedReference);
    miare::detail::writeLittleEndian<std::uint64_t>(
        highWaterBytes / quantum,
        publication,
        miare::detail::PublicationLayout::highWaterBlocks);
    const auto slotIndex = static_cast<std::uint16_t>(opened.format.generation % 2);
    const auto slot = miare::detail::encodePublicationSlot(
        opened, publication, slotIndex, providers);
    file.writeExactAt(
        miare::detail::bootstrapBytes +
            slotIndex * miare::detail::publicationSlotBytes,
        slot);
    file.stableStorageBarrier();
    return file.bytes();
}

void contradictoryAndNoncanonicalCompressionIsCorrupt() {
    const auto uncompressed = committedImage(miare::Compression::None);
    const auto compressedUnderNone = rewriteLeafAsCompressed(uncompressed, false);
    expectImageDatabaseError(compressedUnderNone, miare::Errc::Corrupt);

    const auto compressionEnabled = committedImage(miare::Compression::ZStd);
    const auto sameSpanCompression = rewriteLeafAsCompressed(
        compressionEnabled, true);
    expectImageDatabaseError(sameSpanCompression, miare::Errc::Corrupt);

    const auto oversizedReference = rewriteLeafReferenceSpan(
        compressionEnabled, 5);
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    file->replaceStableBytes(oversizedReference);
    file->failReadsAtOrAfter(miare::detail::commonRegionBytes);
    expectDatabaseError(miare::Errc::Corrupt, [&] {
        (void)miare::testing::DatabaseAccess::open(
            std::move(file),
            miare::EncryptionKeyView{keyBytes},
            deterministicProviders(22));
    });
}

void interruptionsSelectOnlyCompleteGenerations() {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto* fileView = file.get();
    auto database = miare::testing::DatabaseAccess::create(
        std::move(file),
        miare::EncryptionKeyView{keyBytes},
        deterministicProviders(7));
    const auto oldImage = fileView->bytes();
    auto write = database.beginWrite();
    write.put(bytes("atomic"), bytes("new"));
    write.commit();
    const auto newImage = fileView->bytes();

    auto dataOnly = newImage;
    std::copy(oldImage.begin(), oldImage.end(), dataOnly.begin());
    assert(!imageContains(dataOnly, "atomic", "new"));

    constexpr std::array<std::size_t, 5> tornLengths{0, 1, 512, 4095, 4096};
    for (const auto length : tornLengths) {
        auto interrupted = dataOnly;
        std::copy_n(
            newImage.begin() + miare::detail::bootstrapBytes,
            length,
            interrupted.begin() + miare::detail::bootstrapBytes);
        if (length == miare::detail::publicationSlotBytes) {
            assert(imageContains(interrupted, "atomic", "new"));
        } else {
            assert(!imageContains(interrupted, "atomic", "new"));
        }
    }
    assert(imageContains(newImage, "atomic", "new"));

    auto secondWrite = database.beginWrite();
    secondWrite.put(bytes("atomic"), bytes("newest"));
    secondWrite.commit();
    const auto newestImage = fileView->bytes();
    auto secondDataOnly = newestImage;
    std::copy(newImage.begin(), newImage.begin() + miare::detail::commonRegionBytes,
              secondDataOnly.begin());
    assert(imageContains(secondDataOnly, "atomic", "new"));
    for (const auto length : tornLengths) {
        auto interrupted = secondDataOnly;
        std::copy_n(
            newestImage.begin() + miare::detail::bootstrapBytes +
                miare::detail::publicationSlotBytes,
            length,
            interrupted.begin() + miare::detail::bootstrapBytes +
                miare::detail::publicationSlotBytes);
        assert(imageContains(
            interrupted,
            "atomic",
            length == 4096 ? "newest" : "new"));
    }

    auto damagedCandidate = newestImage;
    damagedCandidate[newImage.size() + 160] ^= std::byte{1};
    expectImageDatabaseError(damagedCandidate, miare::Errc::Corrupt);
    database.close();
}

void handlesEnforceAdmissionAffinityAndPreflightGuarantees() {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto crypto = std::make_unique<miare::testing::DeterministicCryptoProvider>(9);
    auto* cryptoView = crypto.get();
    auto providers = miare::detail::ProviderAccess::make(
        std::move(crypto),
        std::make_unique<miare::testing::FaultInjectingCompressionProvider>());
    auto database = miare::testing::DatabaseAccess::create(
        std::move(file),
        miare::EncryptionKeyView{keyBytes},
        std::move(providers));

    auto writer = database.beginWrite();
    auto busy = database.tryBeginWrite();
    assert(!busy);
    assert(busy.error() == miare::WriterBusy{});
    writer.rollback();

    auto read = database.beginRead();
    std::atomic<bool> wrongThreadRejected{false};
    std::thread other([&] {
        try {
            (void)read.contains(bytes("key"));
        } catch (const miare::ContractError& error) {
            wrongThreadRejected.store(
                error.code() == miare::Errc::WrongThread,
                std::memory_order_relaxed);
        }
    });
    other.join();
    assert(wrongThreadRejected.load(std::memory_order_relaxed));
    assert(!read.contains(bytes("key")));
    read.end();

    auto preflight = database.beginWrite();
    preflight.put(bytes("key"), bytes("value"));
    cryptoView->failNextRandom();
    expectDatabaseError(miare::Errc::ProviderUnavailable, [&] {
        preflight.commit();
    });
    assert(preflight.active());
    assert(database.state() == miare::DatabaseState::Open);
    assert(equals(preflight.get(bytes("key")), "value"));
    preflight.rollback();

    auto movable = database.beginRead();
    auto moved = std::move(movable);
    assert(!movable.active());
    expectContractError(miare::Errc::InvalidState, [&] {
        (void)movable.contains(bytes("key"));
    });
    moved.end();
    database.close();
}

void closedDatabaseInvalidatesWriteHandle() {
#ifdef NDEBUG
    auto counts = std::make_shared<AllocationCounts>();
    CountingAllocator<std::byte> allocator{counts};
    using AllocatedDatabase = miare::Database<CountingAllocator<std::byte>>;
    std::optional<typename AllocatedDatabase::WriteTransaction> write;
    std::size_t beforeDestruction = 0;
    {
        auto file = std::make_unique<miare::testing::MemoryDurableFile>();
        auto database = miare::testing::DatabaseAccess::create(
            std::move(file),
            miare::EncryptionKeyView{keyBytes},
            deterministicProviders(17),
            miare::CreateOptions{},
            allocator);
        write.emplace(database.beginWrite());
        beforeDestruction = counts->deallocatedBytes.load(
            std::memory_order_relaxed);
    }
    requireTest(counts->deallocatedBytes.load(std::memory_order_relaxed) >
                beforeDestruction);
    requireTest(!write->active());
    bool invalidState = false;
    try {
        (void)write->get(bytes("key"));
    } catch (const miare::ContractError& error) {
        invalidState = error.code() == miare::Errc::InvalidState;
    }
    requireTest(invalidState);
    write->rollback();
#endif
}

int liveChildDestructionProbe() {
#ifdef NDEBUG
    return 77;
#else
    std::optional<DefaultWrite> write;
    {
        auto file = std::make_unique<miare::testing::MemoryDurableFile>();
        auto database = miare::testing::DatabaseAccess::create(
            std::move(file),
            miare::EncryptionKeyView{keyBytes},
            deterministicProviders(28));
        write.emplace(database.beginWrite());
    }
    return 0;
#endif
}

void closeErasesDerivedKeys() {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto database = miare::testing::DatabaseAccess::create(
        std::move(file),
        miare::EncryptionKeyView{keyBytes},
        deterministicProviders(23));
    assert(!miare::testing::DatabaseAccess::sessionKeysErased(database));
    database.close();
    assert(miare::testing::DatabaseAccess::sessionKeysErased(database));
}

void writerAdmissionIsFifo() {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto database = miare::testing::DatabaseAccess::create(
        std::move(file),
        miare::EncryptionKeyView{keyBytes},
        deterministicProviders(10));
    auto admitted = database.beginWrite();
    std::mutex orderMutex;
    std::vector<int> order;
    const auto enqueue = [&](int identity) {
        auto queued = database.beginWrite();
        {
            std::lock_guard lock{orderMutex};
            order.push_back(identity);
        }
        queued.rollback();
    };
    const auto waitForQueuedWriters = [&](std::size_t expected) {
        return waitUntil([&] {
            return miare::testing::DatabaseAccess::waitingWriters(database) ==
                expected;
        });
    };
    std::thread first(enqueue, 1);
    if (!waitForQueuedWriters(1)) {
        admitted.rollback();
        first.join();
        requireTest(false);
    }
    std::thread second(enqueue, 2);
    if (!waitForQueuedWriters(2)) {
        admitted.rollback();
        first.join();
        second.join();
        requireTest(false);
    }
    admitted.rollback();
    first.join();
    second.join();
    requireTest((order == std::vector<int>{1, 2}));
    database.close();
}

void closeCancelsQueuedWriterAdmission() {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto database = miare::testing::DatabaseAccess::create(
        std::move(file),
        miare::EncryptionKeyView{keyBytes},
        deterministicProviders(26));
    auto admitted = database.beginWrite();
    std::atomic<bool> cancelled{false};
    std::atomic<bool> finished{false};
    std::thread queued([&] {
        try {
            auto unexpected = database.beginWrite();
            unexpected.rollback();
        } catch (const miare::ContractError& error) {
            cancelled.store(
                error.code() == miare::Errc::InvalidState,
                std::memory_order_release);
        }
        finished.store(true, std::memory_order_release);
    });
    if (!waitUntil([&] {
            return miare::testing::DatabaseAccess::waitingWriters(database) == 1;
        })) {
        admitted.rollback();
        queued.join();
        requireTest(false);
    }
    bool liveChildren = false;
    try {
        database.close();
    } catch (const miare::ContractError& error) {
        liveChildren = error.code() == miare::Errc::LiveChildren;
    }
    requireTest(liveChildren);

    if (!waitUntil([&] { return finished.load(std::memory_order_acquire); })) {
        admitted.rollback();
        queued.join();
        requireTest(false);
    }
    queued.join();
    requireTest(cancelled.load(std::memory_order_acquire));
    admitted.rollback();
    database.close();
}

void closePreservesConcurrentRecoveryTransition() {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto database = miare::testing::DatabaseAccess::create(
        std::move(file),
        miare::EncryptionKeyView{keyBytes},
        deterministicProviders(27));
    auto writer = database.beginWrite();
    auto sessionLock = miare::testing::DatabaseAccess::lockSession(database);
    std::atomic<bool> liveChildren{false};
    std::thread closer([&] {
        try {
            database.close();
        } catch (const miare::ContractError& error) {
            liveChildren.store(
                error.code() == miare::Errc::LiveChildren,
                std::memory_order_release);
        }
    });
    while (database.state() != miare::DatabaseState::Closing) {
        std::this_thread::yield();
    }
    miare::testing::DatabaseAccess::forceRecoveryRequired(database);
    sessionLock.unlock();
    closer.join();
    assert(liveChildren.load(std::memory_order_acquire));
    assert(database.state() == miare::DatabaseState::RecoveryRequired);
    writer.rollback();
    database.close();
}

void readersObserveCompleteGenerationsDuringCommit() {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto database = miare::testing::DatabaseAccess::create(
        std::move(file),
        miare::EncryptionKeyView{keyBytes},
        deterministicProviders(24));
    std::string previous;
    for (unsigned generation = 1; generation != 21; ++generation) {
        const auto next = std::to_string(generation);
        auto write = database.beginWrite();
        write.put(bytes("generation"), bytes(next.c_str()));
        std::atomic<bool> stop{false};
        std::atomic<unsigned> observingReaders{0};
        std::vector<std::thread> readers;
        for (unsigned index = 0; index != 4; ++index) {
            readers.emplace_back([&] {
                bool observed = false;
                while (!stop.load(std::memory_order_acquire)) {
                    auto read = database.beginRead();
                    const auto value = read.get(bytes("generation"));
                    const bool predecessor = previous.empty()
                        ? !value
                        : equals(value, previous.c_str());
                    const bool successor = equals(value, next.c_str());
                    assert(predecessor || successor);
                    read.end();
                    if (!observed) {
                        observed = true;
                        observingReaders.fetch_add(1, std::memory_order_release);
                    }
                }
            });
        }
        requireTest(waitUntil([&] {
            return observingReaders.load(std::memory_order_acquire) ==
                readers.size();
        }));
        write.commit();
        stop.store(true, std::memory_order_release);
        for (auto& reader : readers) {
            reader.join();
        }
        previous = next;
    }
    database.close();
}

void tryWriterPreservesBlockingAdmissionSequence() {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto database = miare::testing::DatabaseAccess::create(
        std::move(file),
        miare::EncryptionKeyView{keyBytes},
        deterministicProviders(18));
    auto immediate = database.tryBeginWrite();
    assert(immediate);
    immediate.value().rollback();
    auto blocking = database.beginWrite();
    blocking.rollback();
    database.close();
}

void openOptionsBoundReaderAdmission(const TemporaryDirectory& temporary) {
    const auto path = temporary.path() / "reader-budget.miare";
    auto created = miare::Database<>::create(
        path,
        miare::EncryptionKeyView{keyBytes},
        deterministicProviders(11));
    created.close();

    miare::OpenOptions oneReader;
    oneReader.maxReaders = 1;
    auto opened = miare::Database<>::open(
        path,
        miare::EncryptionKeyView{keyBytes},
        deterministicProviders(12),
        oneReader);
    assert(opened);
    auto reader = opened.value().beginRead();
    expectDatabaseError(miare::Errc::ResourceLimit, [&] {
        (void)opened.value().beginRead();
    });
    reader.end();
    opened.value().close();

    miare::OpenOptions tooManyReaders;
    tooManyReaders.maxReaders = 65'536;
    expectContractError(miare::Errc::InvalidConfiguration, [&] {
        (void)miare::Database<>::open(
            path,
            miare::EncryptionKeyView{keyBytes},
            deterministicProviders(13),
            tooManyReaders);
    });
    miare::OpenOptions tooLittleCache;
    tooLittleCache.cacheCapacityBytes = 4U * 1024U * 1024U - 1;
    expectContractError(miare::Errc::InvalidConfiguration, [&] {
        (void)miare::Database<>::open(
            path,
            miare::EncryptionKeyView{keyBytes},
            deterministicProviders(14),
            tooLittleCache);
    });
}

void transactionStateUsesTheDatabaseAllocator() {
    auto counts = std::make_shared<AllocationCounts>();
    CountingAllocator<std::byte> allocator{counts};
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto* fileView = file.get();
    const auto beforeSession = counts->allocatedBytes.load(std::memory_order_relaxed);
    auto database = miare::testing::DatabaseAccess::create(
        std::move(file),
        miare::EncryptionKeyView{keyBytes},
        deterministicProviders(15),
        miare::CreateOptions{},
        allocator);
    requireTest(
        counts->allocatedBytes.load(std::memory_order_relaxed) > beforeSession);
    const auto beforeMutation = counts->allocatedBytes.load(std::memory_order_relaxed);
    auto write = database.beginWrite();
    write.put(bytes("allocated-key"), bytes("allocated-value"));
    assert(counts->allocatedBytes.load(std::memory_order_relaxed) > beforeMutation);
    counts->largestAllocationBytes.store(0, std::memory_order_relaxed);
    write.commit();
    assert(counts->largestAllocationBytes.load(std::memory_order_relaxed) >=
           15U * 1024U);
    const auto beforeRead = counts->allocatedBytes.load(std::memory_order_relaxed);
    auto read = database.beginRead();
    assert(counts->allocatedBytes.load(std::memory_order_relaxed) > beforeRead);
    auto value = read.get(bytes("allocated-key"));
    assert(value);
    assert(value->get_allocator().counts == counts);
    read.end();
    const auto durableImage = fileView->bytes();
    database.close();

    auto reopenedFile = std::make_unique<miare::testing::MemoryDurableFile>();
    reopenedFile->writeExactAt(0, durableImage);
    reopenedFile->stableStorageBarrier();
    counts->largestAllocationBytes.store(0, std::memory_order_relaxed);
    auto reopenedResult = miare::testing::DatabaseAccess::open(
        std::move(reopenedFile),
        miare::EncryptionKeyView{keyBytes},
        deterministicProviders(16),
        allocator);
    assert(reopenedResult);
    auto reopened = std::move(reopenedResult).value();
    assert(counts->largestAllocationBytes.load(std::memory_order_relaxed) >=
           15U * 1024U);
    reopened.close();
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view{argv[1]} == "--assert-live-child") {
        return liveChildDestructionProbe();
    }
    TemporaryDirectory temporary;
    runTestCase("atomic durability", [&] {
        exactMutationsAreAtomicAndDurable(temporary);
    });
    runTestCase("rollback and validation", [&] {
        rollbackAndValidationPreserveCommittedState(temporary);
    });
    runTestCase("durability barriers and fail-stop", [&] {
        commitUsesTwoDurabilityBarriersAndFailsStop();
    });
    runTestCase("compression canonicality", [&] {
        contradictoryAndNoncanonicalCompressionIsCorrupt();
    });
    runTestCase("generation-one canonicality", [&] {
        generationOneMustBeTheCanonicalEmptyDatabase();
    });
    runTestCase("interruption selection", [&] {
        interruptionsSelectOnlyCompleteGenerations();
    });
    runTestCase("handle contracts", [&] {
        handlesEnforceAdmissionAffinityAndPreflightGuarantees();
    });
    runTestCase("database destruction", [&] {
        closedDatabaseInvalidatesWriteHandle();
    });
    runTestCase("key erasure", [&] { closeErasesDerivedKeys(); });
    runTestCase("immediate writer admission", [&] {
        tryWriterPreservesBlockingAdmissionSequence();
    });
    runTestCase("FIFO writer admission", [&] { writerAdmissionIsFifo(); });
    runTestCase("close cancellation", [&] {
        closeCancelsQueuedWriterAdmission();
    });
    runTestCase("concurrent recovery transition", [&] {
        closePreservesConcurrentRecoveryTransition();
    });
    runTestCase("reader publication observations", [&] {
        readersObserveCompleteGenerationsDuringCommit();
    });
    runTestCase("reader admission budget", [&] {
        openOptionsBoundReaderAdmission(temporary);
    });
    runTestCase("allocator routing", [&] {
        transactionStateUsesTheDatabaseAllocator();
    });
}
