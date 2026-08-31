#pragma once

#include <miare/error.hpp>
#include <miare/types.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace miare::detail {

class Blake2b {
public:
    explicit Blake2b(std::size_t outputBytes) : outputBytes_(outputBytes) {
        state_ = initializationVector;
        state_[0] ^= 0x01010000U ^ outputBytes;
    }

    void update(ByteView input) {
        while (!input.empty()) {
            if (bufferBytes_ == buffer_.size()) {
                incrementCounter(buffer_.size());
                compress(false);
                bufferBytes_ = 0;
            }
            const auto count = std::min(buffer_.size() - bufferBytes_, input.size());
            std::copy_n(input.begin(), count, buffer_.begin() + bufferBytes_);
            bufferBytes_ += count;
            input = input.subspan(count);
        }
    }

    void finish(MutableByteView output) {
        if (output.size() != outputBytes_) {
            throw ContractError{Errc::InvalidArgument, "BLAKE2b output has an invalid size"};
        }
        incrementCounter(bufferBytes_);
        std::fill(buffer_.begin() + bufferBytes_, buffer_.end(), std::byte{0});
        compress(true);
        std::array<std::byte, 64> encoded{};
        for (std::size_t word = 0; word != state_.size(); ++word) {
            for (std::size_t byte = 0; byte != 8; ++byte) {
                encoded[word * 8 + byte] = std::byte{
                    static_cast<unsigned char>(state_[word] >> (byte * 8U))};
            }
        }
        std::copy_n(encoded.begin(), output.size(), output.begin());
    }

private:
    static constexpr std::array<std::uint64_t, 8> initializationVector{
        0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
        0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
        0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
        0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};

    static constexpr std::array<std::array<unsigned char, 16>, 12> permutations{{
        {{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}},
        {{14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3}},
        {{11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4}},
        {{7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8}},
        {{9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13}},
        {{2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9}},
        {{12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11}},
        {{13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10}},
        {{6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5}},
        {{10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0}},
        {{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}},
        {{14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3}},
    }};

    static std::uint64_t load64(const std::byte* input) noexcept {
        std::uint64_t result = 0;
        for (std::size_t index = 0; index != 8; ++index) {
            result |= static_cast<std::uint64_t>(
                          std::to_integer<unsigned char>(input[index]))
                << (index * 8U);
        }
        return result;
    }

    static void mix(
        std::array<std::uint64_t, 16>& work,
        std::size_t a,
        std::size_t b,
        std::size_t c,
        std::size_t d,
        std::uint64_t x,
        std::uint64_t y) noexcept {
        work[a] = work[a] + work[b] + x;
        work[d] = std::rotr(work[d] ^ work[a], 32);
        work[c] += work[d];
        work[b] = std::rotr(work[b] ^ work[c], 24);
        work[a] = work[a] + work[b] + y;
        work[d] = std::rotr(work[d] ^ work[a], 16);
        work[c] += work[d];
        work[b] = std::rotr(work[b] ^ work[c], 63);
    }

    void incrementCounter(std::size_t bytes) noexcept {
        const auto previous = counterLow_;
        counterLow_ += bytes;
        if (counterLow_ < previous) {
            ++counterHigh_;
        }
    }

    void compress(bool last) noexcept {
        std::array<std::uint64_t, 16> message{};
        for (std::size_t index = 0; index != message.size(); ++index) {
            message[index] = load64(buffer_.data() + index * 8);
        }
        std::array<std::uint64_t, 16> work{};
        std::copy(state_.begin(), state_.end(), work.begin());
        std::copy(initializationVector.begin(), initializationVector.end(), work.begin() + 8);
        work[12] ^= counterLow_;
        work[13] ^= counterHigh_;
        if (last) {
            work[14] = ~work[14];
        }
        for (const auto& permutation : permutations) {
            mix(work, 0, 4, 8, 12, message[permutation[0]], message[permutation[1]]);
            mix(work, 1, 5, 9, 13, message[permutation[2]], message[permutation[3]]);
            mix(work, 2, 6, 10, 14, message[permutation[4]], message[permutation[5]]);
            mix(work, 3, 7, 11, 15, message[permutation[6]], message[permutation[7]]);
            mix(work, 0, 5, 10, 15, message[permutation[8]], message[permutation[9]]);
            mix(work, 1, 6, 11, 12, message[permutation[10]], message[permutation[11]]);
            mix(work, 2, 7, 8, 13, message[permutation[12]], message[permutation[13]]);
            mix(work, 3, 4, 9, 14, message[permutation[14]], message[permutation[15]]);
        }
        for (std::size_t index = 0; index != state_.size(); ++index) {
            state_[index] ^= work[index] ^ work[index + 8];
        }
    }

    std::array<std::uint64_t, 8> state_{};
    std::array<std::byte, 128> buffer_{};
    std::uint64_t counterLow_ = 0;
    std::uint64_t counterHigh_ = 0;
    std::size_t bufferBytes_ = 0;
    std::size_t outputBytes_;
};

template<std::size_t OutputBytes, class... Inputs>
[[nodiscard]] inline std::array<std::byte, OutputBytes> blake2b(Inputs... inputs) {
    std::array<std::byte, OutputBytes> output{};
    Blake2b hash{OutputBytes};
    (hash.update(ByteView{inputs}), ...);
    hash.finish(output);
    return output;
}

} // namespace miare::detail
