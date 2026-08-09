#include <miare/database.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <system_error>

int main() {
    const auto suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto directory = std::filesystem::temp_directory_path() /
        ("miare-package-consumer-" + suffix);
    const auto createdPath = directory / "created.miare";
    const auto movedPath = directory / "moved.miare";
    std::filesystem::create_directory(directory);

    try {
        std::array<std::byte, 32> keyBytes{};
        const miare::EncryptionKeyView key{keyBytes};
        auto database = miare::Database<>::create(
            createdPath, key, miare::ProviderSet::system());
        database.close();
        std::filesystem::rename(createdPath, movedPath);
        auto reopened = miare::Database<>::open(
            movedPath, key, miare::ProviderSet::system());
        if (!reopened) {
            return 1;
        }
        reopened.value().close();
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
        return 1;
    }

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    return 0;
}
