#include <miare/database.hpp>
#include <miare/testing/fakes.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <map>
#include <optional>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

constexpr std::array<std::byte, 32> encryptionKey{};

using ReadCursor = miare::Database<>::ReadCursor;
using WriteCursor = miare::Database<>::WriteCursor;
static_assert(std::is_move_constructible_v<ReadCursor>);
static_assert(!std::is_default_constructible_v<ReadCursor>);
static_assert(!std::is_copy_constructible_v<ReadCursor>);
static_assert(!std::is_move_assignable_v<ReadCursor>);
static_assert(std::is_move_constructible_v<WriteCursor>);
static_assert(!std::is_default_constructible_v<WriteCursor>);
static_assert(!std::is_copy_constructible_v<WriteCursor>);
static_assert(!std::is_move_assignable_v<WriteCursor>);

[[nodiscard]] miare::ProviderSet deterministicProviders(std::uint64_t seed) {
    return miare::detail::ProviderAccess::make(
        std::make_unique<miare::testing::DeterministicCryptoProvider>(seed),
        std::make_unique<miare::testing::FaultInjectingCompressionProvider>());
}

[[nodiscard]] std::vector<std::byte> orderedKey(std::uint16_t index) {
    std::vector<std::byte> key(600, std::byte{0x5a});
    key[0] = std::byte{static_cast<unsigned char>(index >> 8U)};
    key[1] = std::byte{static_cast<unsigned char>(index)};
    return key;
}

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

[[nodiscard]] std::vector<std::byte> key(
    std::initializer_list<unsigned char> bytes) {
    std::vector<std::byte> result;
    result.reserve(bytes.size());
    for (const auto byte : bytes) {
        result.push_back(std::byte{byte});
    }
    return result;
}

template<class Cursor>
[[nodiscard]] std::vector<std::vector<std::byte>> collectForward(
    Cursor& cursor) {
    std::vector<std::vector<std::byte>> result;
    for (bool found = cursor.first(); found; found = cursor.next()) {
        result.emplace_back(cursor.key().begin(), cursor.key().end());
    }
    return result;
}

[[nodiscard]] bool bytesEqual(miare::ByteView left, miare::ByteView right) {
    return left.size() == right.size() &&
        std::equal(left.begin(), left.end(), right.begin());
}

void cursorsTraverseUnsignedByteOrderAcrossLeaves() {
    auto database = miare::testing::DatabaseAccess::create(
        std::make_unique<miare::testing::MemoryDurableFile>(),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(1));
    auto write = database.beginWrite();
    for (std::uint16_t index = 0; index != 80; ++index) {
        const auto key = orderedKey(index);
        const std::array value{
            std::byte{static_cast<unsigned char>(index & 0xffU)}};
        write.put(key, value);
    }
    write.commit();

    auto read = database.beginRead();
    auto cursor = read.scan();
    assert(!cursor.positioned());
    for (std::uint16_t index = 0; index != 80; ++index) {
        assert(index == 0 ? cursor.first() : cursor.next());
        const auto expected = orderedKey(index);
        assert(std::equal(cursor.key().begin(), cursor.key().end(), expected.begin()));
        assert(cursor.value().front() ==
            std::byte{static_cast<unsigned char>(index & 0xffU)});
    }
    assert(!cursor.next());
    assert(!cursor.positioned());

    for (std::uint16_t index = 80; index != 0; --index) {
        assert(index == 80 ? cursor.last() : cursor.previous());
        const auto expected = orderedKey(index - 1);
        assert(std::equal(cursor.key().begin(), cursor.key().end(), expected.begin()));
    }
    assert(!cursor.previous());
    auto moved = std::move(cursor);
    assert(!cursor.positioned());
    assert(!moved.positioned());
}

void prefixAndHalfOpenRangesHonorByteBoundaries() {
    auto database = miare::testing::DatabaseAccess::create(
        std::make_unique<miare::testing::MemoryDurableFile>(),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(2));
    const std::vector<std::vector<std::byte>> keys{
        key({}),
        key({0x00}),
        key({0x01}),
        key({0x01, 0xff}),
        key({0x02}),
        key({0x80}),
        key({0xff}),
        key({0xff, 0x00}),
        key({0xff, 0xff}),
        key({0xff, 0xff, 0x00})};
    auto write = database.beginWrite();
    for (const auto& item : keys) {
        write.put(item, item);
    }
    write.commit();

    auto read = database.beginRead();
    auto allPrefix = read.scan(miare::KeyRangeView::prefix({}));
    assert(collectForward(allPrefix) == keys);

    const auto onePrefix = key({0x01});
    auto one = read.scan(miare::KeyRangeView::prefix(onePrefix));
    assert(collectForward(one) ==
        std::vector<std::vector<std::byte>>({key({0x01}), key({0x01, 0xff})}));

    const auto allFfPrefix = key({0xff, 0xff});
    auto allFf = read.scan(miare::KeyRangeView::prefix(allFfPrefix));
    assert(collectForward(allFf) == std::vector<std::vector<std::byte>>({
        key({0xff, 0xff}), key({0xff, 0xff, 0x00})}));

    auto openLower = read.scan(miare::KeyRangeView::halfOpen(
        std::nullopt, miare::ByteView{onePrefix}));
    assert(collectForward(openLower) ==
        std::vector<std::vector<std::byte>>({key({}), key({0x00})}));

    const auto lower = key({0x01});
    const auto upper = key({0x80});
    auto bounded = read.scan(miare::KeyRangeView::halfOpen(lower, upper));
    assert(collectForward(bounded) == std::vector<std::vector<std::byte>>({
        key({0x01}), key({0x01, 0xff}), key({0x02})}));

    const auto presentEmpty = key({});
    auto emptyUpper = read.scan(miare::KeyRangeView::halfOpen(
        std::nullopt, miare::ByteView{presentEmpty}));
    assert(!emptyUpper.first());
    auto equalBounds = read.scan(miare::KeyRangeView::halfOpen(lower, lower));
    assert(!equalBounds.last());
    auto reversed = read.scan(miare::KeyRangeView::halfOpen(upper, lower));
    assert(!reversed.first());
}

void positioningCopiesBoundsAndPreservesViewsUntilMovement() {
    auto database = miare::testing::DatabaseAccess::create(
        std::make_unique<miare::testing::MemoryDurableFile>(),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(3));
    auto write = database.beginWrite();
    const auto a = key({0x10});
    const auto b = key({0x20});
    const auto c = key({0x30});
    write.put(a, key({0xa0}));
    write.put(b, key({0xb0}));
    write.put(c, key({0xc0}));
    write.commit();

    auto read = database.beginRead();
    auto mutableLower = a;
    auto mutableUpper = c;
    auto cursor = read.scan(miare::KeyRangeView::halfOpen(
        mutableLower, mutableUpper));
    mutableLower.front() = std::byte{0xff};
    mutableUpper.front() = std::byte{0x00};
    assert(cursor.seekLowerBound(key({0x11})));
    assert(bytesEqual(cursor.key(), b));
    const auto keyView = cursor.key();
    const auto valueView = cursor.value();
    assert(bytesEqual(keyView, b));
    assert(bytesEqual(valueView, key({0xb0})));
    assert(!cursor.next());
    assert(!cursor.positioned());
    expectContractError(miare::Errc::InvalidState, [&] {
        (void)cursor.key();
    });
    expectContractError(miare::Errc::InvalidState, [&] {
        (void)cursor.previous();
    });

    assert(cursor.first());
    auto movedCursor = std::move(cursor);
    assert(!cursor.positioned());
    expectContractError(miare::Errc::InvalidState, [&] {
        (void)cursor.key();
    });
    const auto beforeInvalidSeek = movedCursor.key();
    std::vector<std::byte> oversized(miare::DefaultLimits::maxKeyBytes + 1);
    expectContractError(miare::Errc::InvalidArgument, [&] {
        (void)movedCursor.seekLowerBound(oversized);
    });
    assert(bytesEqual(movedCursor.key(), beforeInvalidSeek));
}

void cursorLifetimeAndWriteMutationRulesAreStable() {
    auto database = miare::testing::DatabaseAccess::create(
        std::make_unique<miare::testing::MemoryDurableFile>(),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(4));
    const auto a = key({0x10});
    const auto b = key({0x20});
    const auto c = key({0x30});
    {
        auto seed = database.beginWrite();
        seed.put(a, key({0xa0}));
        seed.put(b, key({0xb0}));
        seed.commit();
    }

    auto oldRead = database.beginRead();
    auto oldCursor = oldRead.scan();
    assert(oldCursor.first());
    auto movedRead = std::move(oldRead);
    assert(!oldRead.active());
    assert(oldCursor.next());
    assert(bytesEqual(oldCursor.key(), b));

    auto write = database.beginWrite();
    auto beforeMutation = write.scan();
    assert(beforeMutation.first());
    assert(!write.erase(c));
    assert(beforeMutation.positioned());
    write.put(a, key({0xa1}));
    assert(!beforeMutation.positioned());
    expectContractError(miare::Errc::InvalidState, [&] {
        (void)beforeMutation.next();
    });

    write.put(c, key({0xc0}));
    auto current = write.scan();
    assert(current.first());
    const auto aliasedKey = current.key();
    const auto aliasedValue = current.value();
    write.put(aliasedKey, aliasedValue);
    assert(!current.positioned());
    auto finalWriteView = write.scan();
    assert(collectForward(finalWriteView) ==
        std::vector<std::vector<std::byte>>({a, b, c}));
    write.commit();
    assert(!finalWriteView.positioned());
    expectContractError(miare::Errc::InvalidState, [&] {
        (void)finalWriteView.first();
    });

    assert(collectForward(oldCursor) ==
        std::vector<std::vector<std::byte>>({a, b}));
    movedRead.end();
    assert(!oldCursor.positioned());
    expectContractError(miare::Errc::InvalidState, [&] {
        (void)oldCursor.value();
    });

    auto committed = database.beginRead();
    auto committedCursor = committed.scan();
    assert(committedCursor.first());
    miare::Errc crossThreadError = miare::Errc::InvalidState;
    std::thread otherThread([&] {
        try {
            (void)committedCursor.key();
        } catch (const miare::ContractError& error) {
            crossThreadError = error.code();
        }
    });
    otherThread.join();
    assert(crossThreadError == miare::Errc::WrongThread);
    committed.end();
}

void invalidBoundsAreRejectedWithoutChangingTransactions() {
    auto database = miare::testing::DatabaseAccess::create(
        std::make_unique<miare::testing::MemoryDurableFile>(),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(5));
    std::vector<std::byte> maximumKey(
        miare::DefaultLimits::maxKeyBytes,
        std::byte{0x44});
    {
        auto write = database.beginWrite();
        write.put(maximumKey, key({0xaa}));
        write.commit();
    }
    auto read = database.beginRead();
    std::vector<std::byte> invalidBound(
        miare::DefaultLimits::maxKeyBytes + 2,
        std::byte{0x44});
    expectContractError(miare::Errc::InvalidArgument, [&] {
        (void)read.scan(miare::KeyRangeView::halfOpen(
            miare::ByteView{invalidBound}, std::nullopt));
    });
    assert(read.active());
    expectContractError(miare::Errc::InvalidArgument, [&] {
        (void)read.scan(miare::KeyRangeView::prefix(invalidBound));
    });
    assert(read.active());
    auto inclusiveUpper = maximumKey;
    inclusiveUpper.push_back(std::byte{0});
    auto maximumRange = read.scan(miare::KeyRangeView::halfOpen(
        miare::ByteView{maximumKey}, miare::ByteView{inclusiveUpper}));
    assert(maximumRange.first());
    assert(bytesEqual(maximumRange.key(), maximumKey));
    assert(!maximumRange.next());
    read.end();
}

using Model = std::map<std::vector<std::byte>, std::vector<std::byte>>;

template<class Transaction>
void requireMatchesModel(Transaction& transaction, const Model& model) {
    auto cursor = transaction.scan();
    auto expected = model.begin();
    for (bool found = cursor.first(); found; found = cursor.next()) {
        assert(expected != model.end());
        assert(bytesEqual(cursor.key(), expected->first));
        assert(bytesEqual(cursor.value(), expected->second));
        ++expected;
    }
    assert(expected == model.end());
}

[[nodiscard]] std::vector<std::byte> modeledKey(std::uint32_t identity) {
    return key({
        static_cast<unsigned char>((identity * 73U) & 0xffU),
        static_cast<unsigned char>((identity * 151U) & 0xffU),
        static_cast<unsigned char>(identity & 0xffU)});
}

[[nodiscard]] std::vector<std::byte> modeledValue(
    std::uint32_t step,
    std::uint32_t identity) {
    return key({
        static_cast<unsigned char>(step & 0xffU),
        static_cast<unsigned char>(identity & 0xffU)});
}

void modelHistoriesRemainStableThroughRollbackCommitAndReopen() {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto* fileView = file.get();
    auto database = miare::testing::DatabaseAccess::create(
        std::move(file),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(6));
    Model committed;
    std::uint32_t state = 0x6d2b79f5U;
    for (std::uint32_t batch = 0; batch != 12; ++batch) {
        const auto snapshot = committed;
        auto stableRead = database.beginRead();
        auto write = database.beginWrite();
        auto working = committed;
        for (std::uint32_t operation = 0; operation != 24; ++operation) {
            state ^= state << 13U;
            state ^= state >> 17U;
            state ^= state << 5U;
            const auto identity = state % 48U;
            const auto itemKey = modeledKey(identity);
            if ((state >> 8U) % 4U == 0) {
                const auto expected = working.erase(itemKey) != 0;
                assert(write.erase(itemKey) == expected);
            } else {
                auto value = modeledValue(batch * 24U + operation, identity);
                write.put(itemKey, value);
                working.insert_or_assign(itemKey, std::move(value));
            }
            requireMatchesModel(write, working);
            requireMatchesModel(stableRead, committed);
        }
        if (batch % 4U == 1U) {
            write.rollback();
        } else {
            write.commit();
            committed = std::move(working);
        }
        requireMatchesModel(stableRead, snapshot);
        stableRead.end();
        auto latest = database.beginRead();
        requireMatchesModel(latest, committed);
        latest.end();
    }
    const auto image = fileView->bytes();
    database.close();

    auto reopenedFile = std::make_unique<miare::testing::MemoryDurableFile>();
    reopenedFile->replaceStableBytes(image);
    auto reopenedResult = miare::testing::DatabaseAccess::open(
        std::move(reopenedFile),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(7));
    assert(reopenedResult.hasValue());
    auto reopened = std::move(reopenedResult).value();
    auto persisted = reopened.beginRead();
    requireMatchesModel(persisted, committed);
    persisted.end();
    reopened.close();
}

struct TwoCursorLimits : miare::DefaultLimits {
    static constexpr std::uint32_t maxCursorsPerTransaction = 2;
};

void cursorCapacityIsReleasedWithHandles() {
    auto database = miare::testing::DatabaseAccess::create<
        std::allocator<std::byte>, TwoCursorLimits>(
            std::make_unique<miare::testing::MemoryDurableFile>(),
            miare::EncryptionKeyView{encryptionKey},
            deterministicProviders(8));
    auto read = database.beginRead();
    auto first = read.scan();
    {
        auto second = read.scan();
        expectDatabaseError(miare::Errc::ResourceLimit, [&] {
            (void)read.scan();
        });
    }
    auto replacement = read.scan();
    (void)first;
    (void)replacement;
    read.end();

    std::optional<typename decltype(database)::WriteCursor> staleFirst;
    std::optional<typename decltype(database)::WriteCursor> staleSecond;
    {
        auto write = database.beginWrite();
        staleFirst.emplace(write.scan());
        staleSecond.emplace(write.scan());
        write.put(key({0x10}), key({0xaa}));
        assert(!staleFirst->positioned());
        assert(!staleSecond->positioned());
        auto recreated = write.scan();
        assert(recreated.first());
        assert(bytesEqual(recreated.key(), key({0x10})));
    }
}

int liveCursorDestructionProbe() {
    auto database = miare::testing::DatabaseAccess::create(
        std::make_unique<miare::testing::MemoryDurableFile>(),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(9));
    std::optional<ReadCursor> cursor;
    {
        auto read = database.beginRead();
        cursor.emplace(read.scan());
    }
    return 77;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view{argv[1]} == "--assert-live-cursor") {
        return liveCursorDestructionProbe();
    }
    cursorsTraverseUnsignedByteOrderAcrossLeaves();
    prefixAndHalfOpenRangesHonorByteBoundaries();
    positioningCopiesBoundsAndPreservesViewsUntilMovement();
    cursorLifetimeAndWriteMutationRulesAreStable();
    invalidBoundsAreRejectedWithoutChangingTransactions();
    modelHistoriesRemainStableThroughRollbackCommitAndReopen();
    cursorCapacityIsReleasedWithHandles();
}
