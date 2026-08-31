#pragma once

#include <miare/database.hpp>

#if MIARE_HAS_SODIUM
#include <sodium.h>
#endif

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace example {

inline miare::ByteView bytes(std::string_view text) noexcept {
    return {
        reinterpret_cast<const std::byte*>(text.data()),
        text.size()};
}

inline std::string text(miare::ByteView bytes) {
    return {
        reinterpret_cast<const char*>(bytes.data()),
        bytes.size()};
}

#if MIARE_HAS_SODIUM
inline std::array<std::byte, 32> randomKey() {
    if (sodium_init() < 0) {
        throw std::runtime_error{"libsodium initialization failed"};
    }
    std::array<std::byte, 32> key{};
    randombytes_buf(key.data(), key.size());
    return key;
}
#endif

class TemporaryDirectory {
public:
    explicit TemporaryDirectory(std::string_view label)
        : path_(std::filesystem::temp_directory_path() /
              (std::string{label} + "-" +
               std::to_string(
                   std::chrono::steady_clock::now()
                       .time_since_epoch()
                       .count()))) {
        std::filesystem::create_directory(path_);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

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

} // namespace example
