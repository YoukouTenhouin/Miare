#include <miare/database.hpp>
#include <miare/testing/fakes.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace {

constexpr std::array<std::byte, 32> encryptionKey{};
constexpr std::size_t maximumInputBytes = 64U * 1024U;
constexpr std::size_t maximumOperations = 1'024;

[[nodiscard]] miare::ProviderSet providers() {
    return miare::detail::ProviderAccess::make(
        std::make_unique<miare::testing::DeterministicCryptoProvider>(2),
        std::make_unique<miare::testing::FaultInjectingCompressionProvider>());
}

[[nodiscard]] std::array<std::byte, 2> key(std::uint8_t value) {
    return {std::byte{0x6b}, std::byte{static_cast<unsigned char>(value % 3)}};
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(
    const std::uint8_t* data,
    std::size_t size) {
    if (size > maximumInputBytes) {
        return 0;
    }
    try {
        auto database = miare::testing::DatabaseAccess::create(
            std::make_unique<miare::testing::MemoryDurableFile>(),
            miare::EncryptionKeyView{encryptionKey},
            providers());
        const auto input = std::span{data, size};
        const auto operationCount = std::min(size, maximumOperations);
        for (std::size_t index = 0; index != operationCount; ++index) {
            const auto operation = input[index] % 8;
            const auto selectedKey = key(input[index]);
            if (operation <= 3) {
                auto write = database.beginWrite();
                if (operation == 0) {
                    const auto remaining = input.subspan(index + 1);
                    const auto valueBytes = std::min<std::size_t>(
                        remaining.size(), input[index] % 33);
                    write.put(
                        selectedKey,
                        miare::ByteView{
                            reinterpret_cast<const std::byte*>(
                                remaining.data()),
                            valueBytes});
                } else if (operation == 1) {
                    (void)write.erase(selectedKey);
                } else {
                    auto blob = write.createBlob();
                    const auto payload = std::byte{
                        static_cast<unsigned char>(input[index])};
                    blob.write(miare::ByteView{&payload, 1});
                    if (operation == 2) {
                        blob.finish();
                    } else {
                        blob.abort();
                    }
                }
                if ((input[index] & 0x80U) == 0) {
                    write.commit();
                } else {
                    write.rollback();
                }
            } else if (operation == 4) {
                auto read = database.beginRead();
                (void)read.get(selectedKey);
                read.end();
            } else if (operation == 5) {
                database.checkpoint();
            } else if (operation == 6) {
                (void)database.verify();
            } else {
                (void)database.diagnostics();
            }
        }
        database.close();
    } catch (const miare::DatabaseError&) {
    } catch (const miare::ContractError&) {
    }
    return 0;
}
