#include <miare/database.hpp>
#include <miare/testing/fakes.hpp>

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

struct AllocationCounts {
    std::atomic<std::size_t> allocatedBytes{0};
};

template<class T>
class CountingAllocator {
public:
    using value_type = T;

    CountingAllocator() : counts(std::make_shared<AllocationCounts>()) {}
    explicit CountingAllocator(std::shared_ptr<AllocationCounts> sharedCounts)
        : counts(std::move(sharedCounts)) {}

    template<class U>
    CountingAllocator(const CountingAllocator<U>& other) noexcept
        : counts(other.counts) {}

    [[nodiscard]] T* allocate(std::size_t count) {
        counts->allocatedBytes.fetch_add(
            count * sizeof(T), std::memory_order_relaxed);
        return std::allocator<T>{}.allocate(count);
    }

    void deallocate(T* allocation, std::size_t count) noexcept {
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
    expectDatabaseError(miare::Errc::ResourceLimit, [&] {
        invalid.commit();
    });
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
        for (std::size_t attempt = 0; attempt != 1'000'000; ++attempt) {
            if (miare::testing::DatabaseAccess::waitingWriters(database) == expected) {
                return;
            }
            std::this_thread::yield();
        }
        assert(false);
    };
    std::thread first(enqueue, 1);
    waitForQueuedWriters(1);
    std::thread second(enqueue, 2);
    waitForQueuedWriters(2);
    admitted.rollback();
    first.join();
    second.join();
    assert((order == std::vector<int>{1, 2}));
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
    auto database = miare::testing::DatabaseAccess::create(
        std::move(file),
        miare::EncryptionKeyView{keyBytes},
        deterministicProviders(15),
        miare::CreateOptions{},
        allocator);
    const auto beforeMutation = counts->allocatedBytes.load(std::memory_order_relaxed);
    auto write = database.beginWrite();
    write.put(bytes("allocated-key"), bytes("allocated-value"));
    assert(counts->allocatedBytes.load(std::memory_order_relaxed) > beforeMutation);
    write.commit();
    const auto beforeRead = counts->allocatedBytes.load(std::memory_order_relaxed);
    auto read = database.beginRead();
    assert(counts->allocatedBytes.load(std::memory_order_relaxed) > beforeRead);
    auto value = read.get(bytes("allocated-key"));
    assert(value);
    assert(value->get_allocator().counts == counts);
    read.end();
    database.close();
}

} // namespace

int main() {
    TemporaryDirectory temporary;
    exactMutationsAreAtomicAndDurable(temporary);
    rollbackAndValidationPreserveCommittedState(temporary);
    commitUsesTwoDurabilityBarriersAndFailsStop();
    interruptionsSelectOnlyCompleteGenerations();
    handlesEnforceAdmissionAffinityAndPreflightGuarantees();
    writerAdmissionIsFifo();
    openOptionsBoundReaderAdmission(temporary);
    transactionStateUsesTheDatabaseAllocator();
}
