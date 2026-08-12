#include <miare/database.hpp>
#include <miare/detail/database_format.hpp>
#include <miare/testing/fakes.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace {

constexpr std::array<std::byte, 32> encryptionKey{};
constexpr std::size_t maximumInputBytes = 16U * 1024U * 1024U;

[[nodiscard]] miare::ProviderSet providers() {
    return miare::detail::ProviderAccess::make(
        std::make_unique<miare::testing::DeterministicCryptoProvider>(1),
        std::make_unique<miare::testing::FaultInjectingCompressionProvider>());
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(
    const std::uint8_t* data,
    std::size_t size) {
    if (size > maximumInputBytes) {
        return 0;
    }
    std::vector<std::byte> image(
        size < miare::detail::bootstrapBytes
            ? miare::detail::bootstrapBytes
            : size);
    if (size != 0) {
        std::copy_n(
            reinterpret_cast<const std::byte*>(data), size, image.begin());
    }
    auto file = std::make_unique<miare::testing::MemoryDurableFile>();
    file->replaceStableBytes(image);
    try {
        auto opened = miare::testing::DatabaseAccess::open(
            std::move(file),
            miare::EncryptionKeyView{encryptionKey},
            providers());
        if (opened) {
            auto database = std::move(opened).value();
            (void)database.verify();
            database.close();
        }
    } catch (const miare::DatabaseError&) {
    } catch (const miare::ContractError&) {
    }
    return 0;
}
