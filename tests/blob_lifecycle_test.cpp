#include <miare/database.hpp>
#include <miare/testing/fakes.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace {

constexpr std::array<std::byte, 32> encryptionKey{};

struct LifecycleLimits : miare::DefaultLimits {
    static constexpr std::uint64_t blobChunkBytes = 64U * 1024U;
};

using Database = miare::Database<std::allocator<std::byte>, LifecycleLimits>;
using BlobModel = std::map<miare::BlobId, std::vector<std::byte>>;

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

class RandomHistory {
public:
    explicit RandomHistory(std::uint64_t seed) : state_(seed) {}

    [[nodiscard]] std::uint64_t next() noexcept {
        state_ = state_ * 6364136223846793005ULL +
            1442695040888963407ULL;
        return state_;
    }

private:
    std::uint64_t state_;
};

[[nodiscard]] std::vector<std::byte> randomContent(
    RandomHistory& random,
    bool crossChunkBoundary = false) {
    const auto size = crossChunkBoundary
        ? LifecycleLimits::blobChunkBytes + random.next() % 97
        : random.next() % 513;
    std::vector<std::byte> result(static_cast<std::size_t>(size));
    for (auto& value : result) {
        value = std::byte{static_cast<unsigned char>(random.next() >> 56)};
    }
    return result;
}

template<class Transaction>
void verifySnapshot(
    Transaction& transaction,
    const BlobModel& expectedBlobs,
    const std::vector<miare::BlobId>& observedIds,
    const std::optional<std::byte>& expectedEpoch) {
    const auto epoch = transaction.get(bytes("epoch"));
    assert(epoch.has_value() == expectedEpoch.has_value());
    if (expectedEpoch) {
        assert(epoch->size() == 1);
        assert(epoch->front() == *expectedEpoch);
    }
    for (const auto id : observedIds) {
        const auto expected = expectedBlobs.find(id);
        auto reader = transaction.openBlob(id);
        assert(reader.has_value() == (expected != expectedBlobs.end()));
        if (!reader) {
            continue;
        }
        assert(reader->id() == id);
        assert(reader->size() == expected->second.size());
        std::vector<std::byte> actual(expected->second.size());
        assert(reader->read(actual) == actual.size());
        assert(actual == expected->second);
        assert(reader->read(actual) == 0);
        reader->close();
    }
}

[[nodiscard]] miare::BlobId chooseExisting(
    const BlobModel& blobs,
    std::uint64_t selection) {
    auto selected = blobs.begin();
    std::advance(
        selected,
        static_cast<std::ptrdiff_t>(selection % blobs.size()));
    return selected->first;
}

void randomizedKeyAndBlobHistoriesMatchIndependentModel(
    std::uint64_t historyCount,
    std::uint64_t operationsPerHistory) {
    for (std::uint64_t seed = 1; seed <= historyCount; ++seed) {
        RandomHistory random{seed};
        auto file = std::make_unique<miare::testing::MemoryDurableFile>();
        auto* fileView = file.get();
        auto database = miare::testing::DatabaseAccess::create<
            std::allocator<std::byte>, LifecycleLimits>(
            std::move(file),
            miare::EncryptionKeyView{encryptionKey},
            deterministicProviders(seed));
        BlobModel model;
        std::vector<miare::BlobId> observedIds;
        std::optional<std::byte> epoch;

        for (std::uint64_t operation = 0;
             operation != operationsPerHistory;
             ++operation) {
            auto oldSnapshot = database.beginRead();
            const auto oldModel = model;
            const auto oldEpoch = epoch;
            auto candidate = model;
            auto candidateEpoch = std::byte{
                static_cast<unsigned char>((seed + operation) & 0xffU)};
            auto transaction = database.beginWrite();
            transaction.put(
                bytes("epoch"), miare::ByteView{&candidateEpoch, 1});

            auto action = random.next() % 6;
            if (model.empty() || (action == 0 && model.size() < 8)) {
                auto writer = transaction.createBlob();
                const auto id = writer.id();
                observedIds.push_back(id);
                auto content = randomContent(
                    random, operation % 17 == 0);
                writer.write(content);
                writer.finish();
                candidate.insert_or_assign(id, std::move(content));
            } else if (action == 1) {
                const auto id = chooseExisting(model, random.next());
                auto writer = transaction.replaceBlob(id);
                assert(writer);
                auto content = randomContent(
                    random, operation % 19 == 0);
                writer->write(content);
                writer->finish();
                candidate.insert_or_assign(id, std::move(content));
            } else if (action == 2) {
                const auto id = chooseExisting(model, random.next());
                assert(transaction.eraseBlob(id));
                candidate.erase(id);
            } else if (action == 3) {
                auto writer = transaction.createBlob();
                const auto id = writer.id();
                observedIds.push_back(id);
                auto content = randomContent(random, true);
                writer.write(content);
                writer.finish();
                assert(transaction.eraseBlob(id));
            } else if (action == 4) {
                auto writer = transaction.createBlob();
                const auto id = writer.id();
                observedIds.push_back(id);
                auto content = randomContent(random, true);
                writer.write(content);
                writer.abort();
            } else {
                const auto id = chooseExisting(model, random.next());
                auto writer = transaction.replaceBlob(id);
                assert(writer);
                auto content = randomContent(random);
                writer->write(content);
                writer->finish();
                assert(transaction.eraseBlob(id));
                candidate.erase(id);
            }

            const bool commit = random.next() % 4 != 0;
            if (commit) {
                transaction.commit();
                model = std::move(candidate);
                epoch = candidateEpoch;
            } else {
                transaction.rollback();
            }
            verifySnapshot(
                oldSnapshot, oldModel, observedIds, oldEpoch);
            oldSnapshot.end();
            auto current = database.beginRead();
            verifySnapshot(current, model, observedIds, epoch);
            current.end();
            const auto diagnostics = database.diagnostics();
            assert(diagnostics.blobReclaimableBytes <=
                diagnostics.reclaimableBytes);
            assert(diagnostics.blobSnapshotRetainedBytes <=
                diagnostics.snapshotRetainedBytes);
        }

        const auto image = fileView->bytes();
        database.close();
        auto reopenedFile =
            std::make_unique<miare::testing::MemoryDurableFile>();
        reopenedFile->replaceStableBytes(image);
        auto reopened = miare::testing::DatabaseAccess::open<
            std::allocator<std::byte>, LifecycleLimits>(
            std::move(reopenedFile),
            miare::EncryptionKeyView{encryptionKey},
            deterministicProviders(seed + historyCount));
        assert(reopened);
        auto snapshot = reopened.value().beginRead();
        verifySnapshot(snapshot, model, observedIds, epoch);
        snapshot.end();
        reopened.value().close();
    }
}

void applicationReferencesDoNotControlBlobLifetime() {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    auto database = miare::testing::DatabaseAccess::create<
        std::allocator<std::byte>, LifecycleLimits>(
        std::move(file),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(100));
    auto creating = database.beginWrite();
    auto writer = creating.createBlob();
    const auto id = writer.id();
    writer.write(bytes("application-owned reference"));
    writer.finish();
    const auto encodedId = id.toBytes();
    creating.put(bytes("reference"), encodedId);
    creating.commit();

    auto unlinking = database.beginWrite();
    assert(unlinking.erase(bytes("reference")));
    unlinking.commit();
    auto stillPresent = database.beginRead();
    auto reader = stillPresent.openBlob(id);
    assert(reader);
    reader->close();
    stillPresent.end();

    auto erasing = database.beginWrite();
    assert(erasing.eraseBlob(id));
    erasing.commit();
    auto absent = database.beginRead();
    assert(!absent.openBlob(id));
    absent.end();
    database.close();
}

} // namespace

int main(int argc, char** argv) {
    const bool qualification = argc == 2 &&
        std::string_view{argv[1]} == "--qualification";
    if (argc > 2 || (argc == 2 && !qualification)) {
        return 2;
    }
    randomizedKeyAndBlobHistoriesMatchIndependentModel(
        qualification ? 1'000U : 24U,
        qualification ? 10'000U : 48U);
    applicationReferencesDoNotControlBlobLifetime();
}
