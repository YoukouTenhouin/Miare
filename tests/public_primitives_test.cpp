#include <miare/database.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <memory>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

int main() {
    using namespace miare;

    static_assert(std::same_as<ByteView, std::span<const std::byte>>);
    static_assert(std::same_as<MutableByteView, std::span<std::byte>>);
    static_assert(!std::is_default_constructible_v<EncryptionKeyView>);

    std::array<std::byte, 3> bytes{};
    EncryptionKeyView key{bytes};
    assert(key.bytes().data() == bytes.data());
    assert(key.bytes().size() == bytes.size());

    auto success = Result<std::string, AuthenticationFailed>::success("ok");
    assert(success);
    assert(success.value() == "ok");
    try {
        (void)success.error();
        assert(false);
    } catch (const ContractError& error) {
        assert(error.code() == Errc::InvalidState);
    }

    auto failure = Result<std::unique_ptr<int>, WriterBusy>::failure(WriterBusy{});
    assert(!failure.hasValue());
    assert(failure.error() == WriterBusy{});

    const std::error_code native{5, std::system_category()};
    DatabaseError databaseError{Errc::Io, "read failed", native};
    assert(databaseError.code() == Errc::Io);
    assert(databaseError.nativeCode() == native);

    static_assert(static_cast<std::uint16_t>(Errc::InvalidArgument) == 0);
    static_assert(static_cast<std::uint16_t>(Errc::InUse) == 16);

    static_assert(CreateOptions{}.storageBackend == StorageBackend::BTree);
    static_assert(CreateOptions{}.compression == Compression::ZStd);
    static_assert(OpenOptions{}.maxReaders == 256);
    static_assert(DefaultLimits::allocationQuantumBytes == 4096);
    static_assert(std::same_as<
                  Database<>::OwnedBytes,
                  std::vector<std::byte, std::allocator<std::byte>>>);
    static_assert(!std::is_default_constructible_v<Database<>>);
    static_assert(std::is_move_constructible_v<Database<>>);
    static_assert(!std::is_copy_constructible_v<Database<>>);
    static_assert(!std::is_move_assignable_v<Database<>>);

    const std::array<std::byte, BlobId::encodedSize> blobBytes{
        std::byte{0}, std::byte{1}, std::byte{2}, std::byte{3},
        std::byte{4}, std::byte{5}, std::byte{6}, std::byte{7},
        std::byte{8}, std::byte{9}, std::byte{10}, std::byte{11},
        std::byte{12}, std::byte{13}, std::byte{14}, std::byte{15}};
    const auto blob = BlobId::fromBytes(blobBytes);
    assert(blob.toBytes() == blobBytes);
}
