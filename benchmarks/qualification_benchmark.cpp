#include <miare/database.hpp>
#include <miare/testing/fakes.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::array<std::byte, 32> encryptionKey{};

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

struct AllocationState {
    std::atomic<std::uint64_t> currentBytes{0};
    std::atomic<std::uint64_t> peakBytes{0};
};

template<class T>
class CountingAllocator {
public:
    using value_type = T;

    CountingAllocator() : state(std::make_shared<AllocationState>()) {}

    explicit CountingAllocator(std::shared_ptr<AllocationState> sharedState)
        : state(std::move(sharedState)) {}

    CountingAllocator(const CountingAllocator&) noexcept = default;

    CountingAllocator(CountingAllocator&& other) noexcept
        : state(other.state) {}

    template<class U>
    CountingAllocator(const CountingAllocator<U>& other) noexcept
        : state(other.state) {}

    [[nodiscard]] T* allocate(std::size_t count) {
        const auto bytes = static_cast<std::uint64_t>(count * sizeof(T));
        const auto current =
            state->currentBytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;
        auto peak = state->peakBytes.load(std::memory_order_relaxed);
        while (peak < current && !state->peakBytes.compare_exchange_weak(
                   peak, current, std::memory_order_relaxed)) {
        }
        return std::allocator<T>{}.allocate(count);
    }

    void deallocate(T* allocation, std::size_t count) noexcept {
        state->currentBytes.fetch_sub(
            static_cast<std::uint64_t>(count * sizeof(T)),
            std::memory_order_relaxed);
        std::allocator<T>{}.deallocate(allocation, count);
    }

    template<class U>
    friend class CountingAllocator;

    template<class U>
    friend bool operator==(
        const CountingAllocator& left,
        const CountingAllocator<U>& right) noexcept {
        return left.state == right.state;
    }

    std::shared_ptr<AllocationState> state;
};

using BenchmarkAllocator = CountingAllocator<std::byte>;
using BenchmarkDatabase = miare::Database<BenchmarkAllocator>;

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        path_ = std::filesystem::temp_directory_path() /
            ("miare-benchmark-" + std::to_string(
                Clock::now().time_since_epoch().count()));
        if (!std::filesystem::create_directory(path_)) {
            throw std::runtime_error{"could not create benchmark directory"};
        }
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

[[nodiscard]] miare::ProviderSet providers(std::uint64_t seed) {
    return miare::detail::ProviderAccess::make(
        std::make_unique<miare::testing::DeterministicCryptoProvider>(seed),
        std::make_unique<miare::testing::FaultInjectingCompressionProvider>());
}

[[nodiscard]] std::vector<std::byte> key(std::uint64_t value) {
    std::vector<std::byte> result(16, std::byte{0});
    for (std::size_t index = 0; index != sizeof(value); ++index) {
        result[result.size() - 1 - index] =
            std::byte{static_cast<unsigned char>(value >> (index * 8U))};
    }
    return result;
}

[[nodiscard]] double microseconds(Clock::duration duration) {
    return std::chrono::duration<double, std::micro>{duration}.count();
}

[[nodiscard]] double percentile(
    std::vector<double> samples,
    double fraction) {
    std::sort(samples.begin(), samples.end());
    const auto index = static_cast<std::size_t>(std::ceil(
        fraction * static_cast<double>(samples.size()))) - 1;
    return samples[std::min(index, samples.size() - 1)];
}

[[nodiscard]] double mebibytesPerSecond(
    std::uint64_t bytes,
    Clock::duration duration) {
    const auto seconds = std::chrono::duration<double>{duration}.count();
    return static_cast<double>(bytes) / (1024.0 * 1024.0) / seconds;
}

struct Results {
    double cacheHitLookupP50Microseconds = 0;
    double cacheHitLookupP99Microseconds = 0;
    double transaction100P50Milliseconds = 0;
    double transaction100P99Milliseconds = 0;
    double orderedScanMebibytesPerSecond = 0;
    double blobWriteMebibytesPerSecond = 0;
    double blobReadMebibytesPerSecond = 0;
    double verificationMebibytesPerSecond = 0;
    double backupMebibytesPerSecond = 0;
    double fileAmplification = 0;
    std::uint64_t reclaimableBytesBeforeCheckpoint = 0;
    std::uint64_t reclaimableBytesAfterCheckpoint = 0;
    double checkpointMilliseconds = 0;
    double noOpCheckpointP99Milliseconds = 0;
    double cleanCloseMilliseconds = 0;
    std::uint64_t peakDatabaseOwnedBytes = 0;
};

[[nodiscard]] Results runBenchmark(const std::filesystem::path& directory) {
    constexpr std::uint64_t recordCount = 512;
    constexpr std::size_t valueBytes = 4U * 1024U;
    constexpr std::size_t blobBytes = 8U * 1024U * 1024U;
    const auto databasePath = directory / "benchmark.miare";
    const auto backupPath = directory / "benchmark-backup.miare";
    BenchmarkAllocator allocator;
    const auto allocationState = allocator.state;
    auto database = BenchmarkDatabase::create(
        databasePath,
        miare::EncryptionKeyView{encryptionKey},
        providers(1),
        {},
        allocator);

    std::vector<std::byte> value(valueBytes, std::byte{0x41});
    auto seed = database.beginWrite();
    for (std::uint64_t index = 0; index != recordCount; ++index) {
        seed.put(key(index), value);
    }
    seed.commit();

    Results result;
    std::vector<double> lookupSamples;
    lookupSamples.reserve(4'000);
    auto lookup = database.beginRead();
    for (std::uint64_t iteration = 0; iteration != 4'000; ++iteration) {
        const auto selected = key(iteration % recordCount);
        const auto started = Clock::now();
        const auto found = lookup.get(selected);
        lookupSamples.push_back(microseconds(Clock::now() - started));
        require(
            found && found->size() == valueBytes,
            "Cache-hit benchmark lookup returned incorrect data");
    }
    lookup.end();
    result.cacheHitLookupP50Microseconds = percentile(lookupSamples, 0.50);
    result.cacheHitLookupP99Microseconds = percentile(lookupSamples, 0.99);

    std::vector<double> transactionSamples;
    transactionSamples.reserve(20);
    for (std::uint64_t round = 0; round != 20; ++round) {
        auto transaction = database.beginWrite();
        for (std::uint64_t mutation = 0; mutation != 100; ++mutation) {
            value.front() = std::byte{
                static_cast<unsigned char>(round + mutation)};
            transaction.put(key(mutation), value);
        }
        const auto started = Clock::now();
        transaction.commit();
        transactionSamples.push_back(
            microseconds(Clock::now() - started) / 1'000.0);
    }
    result.transaction100P50Milliseconds = percentile(transactionSamples, 0.50);
    result.transaction100P99Milliseconds = percentile(transactionSamples, 0.99);

    auto scan = database.beginRead();
    auto cursor = scan.scan();
    std::uint64_t scannedBytes = 0;
    const auto scanStarted = Clock::now();
    for (bool positioned = cursor.first(); positioned; positioned = cursor.next()) {
        scannedBytes += cursor.key().size() + cursor.value().size();
    }
    const auto scanDuration = Clock::now() - scanStarted;
    scan.end();
    result.orderedScanMebibytesPerSecond =
        mebibytesPerSecond(scannedBytes, scanDuration);

    std::vector<std::byte> blobPayload(blobBytes, std::byte{0x72});
    auto blobTransaction = database.beginWrite();
    auto blobWriter = blobTransaction.createBlob();
    const auto blobId = blobWriter.id();
    const auto blobWriteStarted = Clock::now();
    blobWriter.write(blobPayload);
    blobWriter.finish();
    blobTransaction.commit();
    const auto blobWriteDuration = Clock::now() - blobWriteStarted;
    result.blobWriteMebibytesPerSecond =
        mebibytesPerSecond(blobBytes, blobWriteDuration);

    database.close();
    auto reopened = BenchmarkDatabase::open(
        databasePath,
        miare::EncryptionKeyView{encryptionKey},
        providers(2),
        {},
        allocator);
    require(static_cast<bool>(reopened), "Benchmark database failed to reopen");
    auto activeDatabase = std::move(reopened).value();
    auto blobSnapshot = activeDatabase.beginRead();
    auto blobReader = blobSnapshot.openBlob(blobId);
    require(static_cast<bool>(blobReader), "Benchmark Blob failed to reopen");
    std::vector<std::byte> blobOutput(blobBytes);
    const auto blobReadStarted = Clock::now();
    const auto blobReadBytes = blobReader->read(blobOutput);
    const auto blobReadDuration = Clock::now() - blobReadStarted;
    if (blobReadBytes != blobOutput.size() || blobOutput != blobPayload) {
        throw std::runtime_error{"Blob benchmark read returned incorrect data"};
    }
    blobReader->close();
    blobSnapshot.end();
    result.blobReadMebibytesPerSecond =
        mebibytesPerSecond(blobBytes, blobReadDuration);

    const auto beforeCheckpoint = activeDatabase.diagnostics();
    result.reclaimableBytesBeforeCheckpoint = beforeCheckpoint.reclaimableBytes;
    const auto checkpointStarted = Clock::now();
    activeDatabase.checkpoint();
    result.checkpointMilliseconds =
        microseconds(Clock::now() - checkpointStarted) / 1'000.0;
    const auto afterCheckpoint = activeDatabase.diagnostics();
    result.reclaimableBytesAfterCheckpoint = afterCheckpoint.reclaimableBytes;
    require(
        afterCheckpoint.liveBytes != 0,
        "Benchmark diagnostics reported zero live bytes after seeding");
    result.fileAmplification =
        static_cast<double>(afterCheckpoint.mainFileBytes) /
        static_cast<double>(afterCheckpoint.liveBytes);

    const auto verifyStarted = Clock::now();
    const auto verification = activeDatabase.verify();
    const auto verifyDuration = Clock::now() - verifyStarted;
    require(verification.valid, "Benchmark database verification failed");
    result.verificationMebibytesPerSecond = mebibytesPerSecond(
        afterCheckpoint.mainFileBytes, verifyDuration);

    const auto backupStarted = Clock::now();
    const auto backup = activeDatabase.backupTo(backupPath);
    const auto backupDuration = Clock::now() - backupStarted;
    result.backupMebibytesPerSecond = mebibytesPerSecond(
        backup.destinationFileBytes, backupDuration);

    const auto noOpPath = directory / "no-op-checkpoint.miare";
    auto noOpDatabase = BenchmarkDatabase::create(
        noOpPath,
        miare::EncryptionKeyView{encryptionKey},
        providers(3),
        {},
        allocator);
    std::vector<double> noOpSamples;
    noOpSamples.reserve(100);
    for (std::size_t iteration = 0; iteration != 100; ++iteration) {
        const auto started = Clock::now();
        noOpDatabase.checkpoint();
        noOpSamples.push_back(microseconds(Clock::now() - started) / 1'000.0);
    }
    result.noOpCheckpointP99Milliseconds = percentile(noOpSamples, 0.99);
    noOpDatabase.close();

    const auto closeStarted = Clock::now();
    activeDatabase.close();
    result.cleanCloseMilliseconds =
        microseconds(Clock::now() - closeStarted) / 1'000.0;
    result.peakDatabaseOwnedBytes =
        allocationState->peakBytes.load(std::memory_order_relaxed);
    return result;
}

void writeResults(std::ostream& output, const Results& result) {
    output << std::fixed << std::setprecision(3)
           << "{\n"
           << "  \"schema\": \"miare-v1-qualification-benchmark-1\",\n"
           << "  \"cache_hit_lookup_p50_us\": "
           << result.cacheHitLookupP50Microseconds << ",\n"
           << "  \"cache_hit_lookup_p99_us\": "
           << result.cacheHitLookupP99Microseconds << ",\n"
           << "  \"transaction_100_p50_ms\": "
           << result.transaction100P50Milliseconds << ",\n"
           << "  \"transaction_100_p99_ms\": "
           << result.transaction100P99Milliseconds << ",\n"
           << "  \"ordered_scan_mib_s\": "
           << result.orderedScanMebibytesPerSecond << ",\n"
           << "  \"blob_write_mib_s\": "
           << result.blobWriteMebibytesPerSecond << ",\n"
           << "  \"blob_read_mib_s\": "
           << result.blobReadMebibytesPerSecond << ",\n"
           << "  \"verification_mib_s\": "
           << result.verificationMebibytesPerSecond << ",\n"
           << "  \"backup_mib_s\": "
           << result.backupMebibytesPerSecond << ",\n"
           << "  \"file_amplification\": " << result.fileAmplification << ",\n"
           << "  \"reclaimable_bytes_before_checkpoint\": "
           << result.reclaimableBytesBeforeCheckpoint << ",\n"
           << "  \"reclaimable_bytes_after_checkpoint\": "
           << result.reclaimableBytesAfterCheckpoint << ",\n"
           << "  \"checkpoint_ms\": " << result.checkpointMilliseconds << ",\n"
           << "  \"no_op_checkpoint_p99_ms\": "
           << result.noOpCheckpointP99Milliseconds << ",\n"
           << "  \"clean_close_ms\": " << result.cleanCloseMilliseconds << ",\n"
           << "  \"peak_database_owned_bytes\": "
           << result.peakDatabaseOwnedBytes << "\n"
           << "}\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 1 && argc != 3) {
        return 2;
    }
    if (argc == 3 && std::string_view{argv[1]} != "--output") {
        return 2;
    }
    TemporaryDirectory temporary;
    const auto results = runBenchmark(temporary.path());
    if (argc == 1) {
        writeResults(std::cout, results);
        return 0;
    }
    std::ofstream output{argv[2]};
    if (!output) {
        return 1;
    }
    writeResults(output, results);
}
