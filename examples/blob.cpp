#include "example_support.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <iostream>
#include <span>
#include <string>
#include <utility>
#include <vector>

int main() {
    const example::TemporaryDirectory directory{"miare-blob-example"};
    const auto databasePath = directory.path() / "assets.miare";
    const auto keyBytes = example::randomKey();
    const miare::EncryptionKeyView key{keyBytes};
    const std::string content =
        "Blob content may be much larger than memory and written in chunks.";

    auto database = miare::Database<>::create(
        databasePath, key, miare::ProviderSet::system());

    miare::BlobId blobId = [&] {
        auto write = database.beginWrite();
        auto blob = write.createBlob();
        const auto id = blob.id();
        blob.write(example::bytes(content.substr(0, 24)));
        blob.write(example::bytes(content.substr(24)));
        blob.finish();

        const auto encodedId = id.toBytes();
        write.put(example::bytes("featured-asset"), encodedId);
        write.commit();
        return id;
    }();

    database.close();

    auto opened = miare::Database<>::open(
        databasePath, key, miare::ProviderSet::system());
    assert(opened);
    auto reopened = std::move(opened).value();

    {
        auto read = reopened.beginRead();
        const auto encodedId = read.get(example::bytes("featured-asset"));
        assert(encodedId && encodedId->size() == miare::BlobId::encodedSize);
        blobId = miare::BlobId::fromBytes(
            std::span<const std::byte, miare::BlobId::encodedSize>{
                encodedId->data(), encodedId->size()});

        auto openedBlob = read.openBlob(blobId);
        assert(openedBlob);
        auto blob = std::move(*openedBlob);
        std::vector<std::byte> decoded(static_cast<std::size_t>(blob.size()));
        std::size_t consumed = 0;
        while (consumed != decoded.size()) {
            consumed += blob.read(miare::MutableByteView{decoded}.subspan(consumed));
        }
        assert(example::text(decoded) == content);

        blob.seek(0);
        std::array<std::byte, 4> prefix{};
        assert(blob.read(prefix) == prefix.size());
        assert(example::text(prefix) == "Blob");
        std::cout << "Read " << blob.size() << " bytes from the Blob\n";

        blob.close();
        read.end();
    }

    reopened.close();
}
