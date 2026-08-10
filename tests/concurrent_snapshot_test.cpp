#include <miare/database.hpp>
#include <miare/testing/fakes.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr std::array<std::byte, 32> encryptionKey{
    std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04},
    std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08},
    std::byte{0x09}, std::byte{0x0a}, std::byte{0x0b}, std::byte{0x0c},
    std::byte{0x0d}, std::byte{0x0e}, std::byte{0x0f}, std::byte{0x10},
    std::byte{0x11}, std::byte{0x12}, std::byte{0x13}, std::byte{0x14},
    std::byte{0x15}, std::byte{0x16}, std::byte{0x17}, std::byte{0x18},
    std::byte{0x19}, std::byte{0x1a}, std::byte{0x1b}, std::byte{0x1c},
    std::byte{0x1d}, std::byte{0x1e}, std::byte{0x1f}, std::byte{0x20}};

[[nodiscard]] miare::ByteView bytes(std::string_view value) {
    return {
        reinterpret_cast<const std::byte*>(value.data()),
        value.size()};
}

[[nodiscard]] std::array<std::byte, sizeof(std::uint64_t)> encoded(
    std::uint64_t value) {
    std::array<std::byte, sizeof(value)> result{};
    std::memcpy(result.data(), &value, sizeof(value));
    return result;
}

template<class OwnedBytes>
[[nodiscard]] bool isModeledCommittedValue(
    const std::optional<OwnedBytes>& value,
    std::uint64_t writerIterations) {
    if (!value) {
        return true;
    }
    if (value->size() != sizeof(std::uint64_t)) {
        return false;
    }
    std::uint64_t decoded = 0;
    std::memcpy(&decoded, value->data(), sizeof(decoded));
    return decoded >= 1 && decoded <= writerIterations && decoded % 5 != 0;
}

[[nodiscard]] miare::ProviderSet deterministicProviders(std::uint64_t seed) {
    return miare::detail::ProviderAccess::make(
        std::make_unique<miare::testing::DeterministicCryptoProvider>(seed),
        std::make_unique<miare::testing::FaultInjectingCompressionProvider>());
}

void readersRemainStableAcrossCommittedAndRolledBackWriters(
    unsigned readerCount,
    unsigned writerIterations) {
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    miare::CreateOptions options;
    options.compression = miare::Compression::None;
    auto database = miare::testing::DatabaseAccess::create(
        std::move(file),
        miare::EncryptionKeyView{encryptionKey},
        deterministicProviders(1),
        options);

    const auto longReaderCount = readerCount / 2;
    std::atomic<unsigned> readersReady{0};
    std::atomic<bool> started{false};
    std::atomic<bool> stop{false};
    std::atomic<bool> failed{false};
    std::vector<std::thread> readers;
    readers.reserve(readerCount);
    for (unsigned index = 0; index != readerCount; ++index) {
        readers.emplace_back([&, index] {
            using ReadTransaction = miare::Database<>::ReadTransaction;
            std::optional<ReadTransaction> longSnapshot;
            if (index < longReaderCount) {
                longSnapshot.emplace(database.beginRead());
                if (longSnapshot->get(bytes("generation"))) {
                    failed.store(true, std::memory_order_release);
                }
            }
            readersReady.fetch_add(1, std::memory_order_release);
            while (!started.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            while (!stop.load(std::memory_order_acquire)) {
                if (longSnapshot) {
                    if (longSnapshot->get(bytes("generation"))) {
                        failed.store(true, std::memory_order_release);
                    }
                    std::this_thread::yield();
                    continue;
                }
                auto snapshot = database.beginRead();
                const auto first = snapshot.get(bytes("generation"));
                std::this_thread::yield();
                const auto second = snapshot.get(bytes("generation"));
                if (first != second ||
                    !isModeledCommittedValue(first, writerIterations)) {
                    failed.store(true, std::memory_order_release);
                }
                snapshot.end();
            }
            if (longSnapshot) {
                longSnapshot->end();
            }
        });
    }

    while (readersReady.load(std::memory_order_acquire) != readerCount) {
        std::this_thread::yield();
    }
    started.store(true, std::memory_order_release);
    std::uint64_t lastCommitted = 0;
    for (std::uint64_t iteration = 1; iteration <= writerIterations; ++iteration) {
        auto writer = database.beginWrite();
        const auto value = encoded(iteration);
        writer.put(bytes("generation"), value);
        if (iteration % 5 == 0) {
            writer.rollback();
        } else {
            writer.commit();
            lastCommitted = iteration;
        }
    }
    const auto pressured = database.diagnostics();
    assert(pressured.activeReaders >= longReaderCount);
    assert(pressured.snapshotRetainedBytes > 0);
    stop.store(true, std::memory_order_release);
    for (auto& reader : readers) {
        reader.join();
    }

    assert(!failed.load(std::memory_order_acquire));
    const auto released = database.diagnostics();
    assert(released.activeReaders == 0);
    assert(released.snapshotRetainedBytes == 0);
    assert(released.reclaimableBytes > 0);
    auto finalSnapshot = database.beginRead();
    const auto finalValue = finalSnapshot.get(bytes("generation"));
    assert(finalValue);
    const auto expected = encoded(lastCommitted);
    assert(std::equal(finalValue->begin(), finalValue->end(), expected.begin()));
    finalSnapshot.end();
    database.close();
}

} // namespace

int main(int argc, char** argv) {
    const bool qualification = argc == 2 &&
        std::string_view{argv[1]} == "--qualification";
    readersRemainStableAcrossCommittedAndRolledBackWriters(
        qualification ? 256U : 8U,
        qualification ? 10'000U : 100U);
}
