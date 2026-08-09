#include <miare/detail/durable_file.hpp>
#include <miare/testing/fakes.hpp>

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
    const std::array<miare::testing::RetainedFileRange, 1> retained{{{0, 1}}};
    memory.simulateCrash(retained);
    assert(memory.bytes()[0] == input[0]);
    assert(memory.bytes()[1] == std::byte{0});

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
