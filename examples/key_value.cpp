#include "example_support.hpp"

#include <cassert>
#include <cstddef>
#include <iostream>
#include <string_view>
#include <utility>

int main() {
    const example::TemporaryDirectory directory{"miare-key-value-example"};
    const auto databasePath = directory.path() / "people.miare";
    const auto keyBytes = example::randomKey();
    const miare::EncryptionKeyView key{keyBytes};

    auto database = miare::Database<>::create(
        databasePath, key, miare::ProviderSet::system());

    {
        auto write = database.beginWrite();
        write.put(example::bytes("person:ada"), example::bytes("Ada Lovelace"));
        write.put(example::bytes("person:grace"), example::bytes("Grace Hopper"));
        write.put(example::bytes("setting:theme"), example::bytes("dark"));
        write.commit();
    }

    {
        auto read = database.beginRead();
        const auto ada = read.get(example::bytes("person:ada"));
        assert(ada && example::text(*ada) == "Ada Lovelace");

        std::size_t people = 0;
        {
            auto cursor = read.scan(
                miare::KeyRangeView::prefix(example::bytes("person:")));
            for (bool found = cursor.first(); found; found = cursor.next()) {
                std::cout << example::text(cursor.key()) << " = "
                          << example::text(cursor.value()) << '\n';
                ++people;
            }
        }
        assert(people == 2);
        read.end();
    }

    database.close();

    auto opened = miare::Database<>::open(
        databasePath, key, miare::ProviderSet::system());
    assert(opened);
    auto reopened = std::move(opened).value();
    {
        auto read = reopened.beginRead();
        assert(read.contains(example::bytes("setting:theme")));
        read.end();
    }
    reopened.close();
}
