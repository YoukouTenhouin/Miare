#include <miare/detail/durable_file.hpp>
#include <miare/testing/fakes.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>

namespace {

std::array<std::byte, 4> bytes(unsigned a, unsigned b, unsigned c, unsigned d) {
    return {
        std::byte{static_cast<unsigned char>(a)},
        std::byte{static_cast<unsigned char>(b)},
        std::byte{static_cast<unsigned char>(c)},
        std::byte{static_cast<unsigned char>(d)}};
}

} // namespace

int main() {
    using miare::Errc;
    using miare::detail::NativeDurableFile;
    using miare::testing::MemoryDurableFile;

    MemoryDurableFile memory;
    memory.resize(8);
    const auto input = bytes(1, 2, 3, 4);
    memory.setMaxTransferBytes(1);
    memory.writeExactAt(2, input);
    std::array<std::byte, 4> output{};
    memory.readExactAt(2, output);
    assert(output == input);
    memory.corruptByte(2);
    memory.readExactAt(2, output);
    assert(output[0] == (input[0] ^ std::byte{1}));
    memory.writeExactAt(2, input);
    memory.stableStorageBarrier();
    assert(memory.barrierCount() == 1);

    memory.failAfterTransferredBytes(2);
    try {
        memory.writeExactAt(0, input);
        assert(false);
    } catch (const miare::DatabaseError& error) {
        assert(error.code() == Errc::Io);
    }
    assert(memory.bytes()[0] == input[0]);
    assert(memory.bytes()[1] == input[1]);

    memory.clearFaults();
    memory.failNextResize();
    try {
        memory.resize(4);
        assert(false);
    } catch (const miare::DatabaseError& error) {
        assert(error.code() == Errc::Io);
    }

    memory.clearFaults();
    memory.failNextBarrier();
    try {
        memory.stableStorageBarrier();
        assert(false);
    } catch (const miare::DatabaseError& error) {
        assert(error.code() == Errc::Durability);
    }
    assert(memory.operations().size() == 9);
    assert(memory.operations()[6].kind ==
           miare::testing::DurableFileOperationKind::Write);
    assert(memory.operations()[6].transferredBytes == 2);
    assert(!memory.operations()[6].succeeded);
    assert(memory.operations()[8].kind ==
           miare::testing::DurableFileOperationKind::Barrier);
    assert(!memory.operations()[8].succeeded);

    memory.simulateCrash();
    assert(memory.bytes()[0] == std::byte{0});
    assert(memory.bytes()[1] == std::byte{0});
    memory.writeExactAt(0, input);
    assert(memory.unbarrieredMutationCount() == 4);
    const std::array<std::size_t, 1> retained{{0}};
    memory.simulateCrash(retained);
    assert(memory.bytes()[0] == input[0]);
    assert(memory.bytes()[1] == std::byte{0});

    memory.setMaxTransferBytes(4);
    const auto earlier = bytes(9, 8, 7, 6);
    const auto later = bytes(5, 4, 3, 2);
    memory.writeExactAt(0, earlier);
    memory.writeExactAt(0, later);
    const std::array<std::size_t, 2> reordered{{1, 0}};
    memory.simulateCrash(reordered);
    assert(std::equal(earlier.begin(), earlier.end(), memory.bytes().begin()));

    memory.resize(4);
    const std::array<std::size_t, 1> retainResize{{0}};
    memory.simulateCrash(retainResize);
    assert(memory.bytes().size() == 4);

    const auto suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto path = std::filesystem::temp_directory_path() /
        ("miare-durable-file-" + suffix + ".db");

    {
        auto file = NativeDurableFile::createNew(path);
        file->resize(8);
        file->writeExactAt(2, input);
        file->stableStorageBarrier();
        file->readExactAt(2, output);
        assert(output == input);

        try {
            (void)NativeDurableFile::openExisting(path);
            assert(false);
        } catch (const miare::DatabaseError& error) {
            assert(error.code() == Errc::InUse);
        }
    }

    {
        auto reopened = NativeDurableFile::openExisting(path);
        reopened->readExactAt(2, output);
        assert(output == input);
    }
    std::filesystem::remove(path);
}
