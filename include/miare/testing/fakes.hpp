#pragma once

#include <miare/detail/durable_file.hpp>
#include <miare/detail/providers.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace miare::testing {

class DeterministicCryptoProvider final : public detail::CryptoProvider {
public:
    explicit DeterministicCryptoProvider(std::uint64_t seed) : state_(seed) {}

    void randomBytes(MutableByteView output) override {
        if (failRandom_) {
            failRandom_ = false;
            throw DatabaseError{Errc::ProviderUnavailable, "injected randomness failure"};
        }
        if (output.size() > detail::maxRandomRequestBytes) {
            throw ContractError{Errc::InvalidArgument, "randomness request exceeds its bound"};
        }
        for (auto& byte : output) {
            state_ ^= state_ << 13U;
            state_ ^= state_ >> 7U;
            state_ ^= state_ << 17U;
            byte = std::byte{static_cast<unsigned char>(state_)};
        }
    }

    void deriveDatabaseRoot(
        ByteView callerKey,
        ByteView databaseIdentity,
        ByteView salt,
        std::uint32_t encryptionSuite,
        std::uint32_t derivationVersion,
        MutableByteView output) override {
        failProviderIfRequested();
        delegate_.deriveDatabaseRoot(
            callerKey,
            databaseIdentity,
            salt,
            encryptionSuite,
            derivationVersion,
            output);
    }

    void deriveSubkey(
        ByteView databaseRoot,
        std::uint64_t subkeyId,
        MutableByteView output) override {
        failProviderIfRequested();
        delegate_.deriveSubkey(databaseRoot, subkeyId, output);
    }

    void encryptDetached(
        ByteView key,
        ByteView nonce,
        ByteView plaintext,
        ByteView associatedData,
        MutableByteView ciphertext,
        MutableByteView tag) override {
        failProviderIfRequested();
        delegate_.encryptDetached(
            key, nonce, plaintext, associatedData, ciphertext, tag);
        if (corruptCiphertext_ && !ciphertext.empty()) {
            ciphertext.front() ^= std::byte{1};
            corruptCiphertext_ = false;
        }
    }

    [[nodiscard]] bool decryptDetached(
        ByteView key,
        ByteView nonce,
        ByteView ciphertext,
        ByteView tag,
        ByteView associatedData,
        MutableByteView plaintext) override {
        failProviderIfRequested();
        return delegate_.decryptDetached(
            key, nonce, ciphertext, tag, associatedData, plaintext);
    }

    void failNextRandom() noexcept { failRandom_ = true; }
    void failNextProviderOperation() noexcept { failProvider_ = true; }
    void corruptNextCiphertext() noexcept { corruptCiphertext_ = true; }

private:
    void failProviderIfRequested() {
        if (failProvider_) {
            failProvider_ = false;
            throw DatabaseError{Errc::ProviderUnavailable, "injected provider failure"};
        }
    }

    detail::SodiumCryptoProvider delegate_;
    std::uint64_t state_;
    bool failRandom_ = false;
    bool failProvider_ = false;
    bool corruptCiphertext_ = false;
};

class FaultInjectingCompressionProvider final : public detail::CompressionProvider {
public:
    [[nodiscard]] std::size_t compressBound(std::size_t inputBytes) const override {
        return delegate_.compressBound(inputBytes);
    }

    [[nodiscard]] std::size_t compress(
        ByteView input,
        MutableByteView output) override {
        failProviderIfRequested();
        const auto written = delegate_.compress(input, output);
        if (corruptFrame_ && written != 0) {
            output.front() ^= std::byte{1};
            corruptFrame_ = false;
        }
        return written;
    }

    void decompress(ByteView frame, MutableByteView output) override {
        failProviderIfRequested();
        delegate_.decompress(frame, output);
    }

    void failNextProviderOperation() noexcept { failProvider_ = true; }
    void corruptNextFrame() noexcept { corruptFrame_ = true; }

private:
    void failProviderIfRequested() {
        if (failProvider_) {
            failProvider_ = false;
            throw DatabaseError{Errc::ProviderUnavailable, "injected provider failure"};
        }
    }

    detail::ZstdCompressionProvider delegate_;
    bool failProvider_ = false;
    bool corruptFrame_ = false;
};

class MemoryDurableFile final : public detail::DurableFile {
public:
    void readExactAt(std::uint64_t offset, MutableByteView destination) override {
        if (offset > bytes_.size() || destination.size() > bytes_.size() - offset) {
            throw DatabaseError{Errc::Io, "injected short read"};
        }
        transfer(destination.size(), [&](std::size_t position, std::size_t count) {
            std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset + position),
                        count,
                        destination.begin() + static_cast<std::ptrdiff_t>(position));
        });
    }

    void writeExactAt(std::uint64_t offset, ByteView source) override {
        if (offset > std::numeric_limits<std::size_t>::max() ||
            source.size() > std::numeric_limits<std::size_t>::max() - offset) {
            throw ContractError{Errc::InvalidArgument, "file range is not representable"};
        }
        const auto end = static_cast<std::size_t>(offset) + source.size();
        if (end > bytes_.size()) {
            bytes_.resize(end);
        }
        transfer(source.size(), [&](std::size_t position, std::size_t count) {
            std::copy_n(source.begin() + static_cast<std::ptrdiff_t>(position),
                        count,
                        bytes_.begin() + static_cast<std::ptrdiff_t>(offset + position));
        });
    }

    void resize(std::uint64_t length) override {
        if (failResize_) {
            failResize_ = false;
            throw DatabaseError{Errc::Io, "injected resize failure"};
        }
        if (length > std::numeric_limits<std::size_t>::max()) {
            throw ContractError{Errc::InvalidArgument, "file length is not representable"};
        }
        bytes_.resize(static_cast<std::size_t>(length));
    }

    void stableStorageBarrier() override {
        if (failBarrier_) {
            failBarrier_ = false;
            throw DatabaseError{Errc::Durability, "injected barrier failure"};
        }
        ++barrierCount_;
    }

    void setMaxTransferBytes(std::size_t bytes) {
        if (bytes == 0) {
            throw ContractError{Errc::InvalidArgument, "transfer size must be positive"};
        }
        maxTransferBytes_ = bytes;
    }

    void failAfterTransferredBytes(std::size_t bytes) { failAfterBytes_ = bytes; }
    void failNextBarrier() noexcept { failBarrier_ = true; }
    void failNextResize() noexcept { failResize_ = true; }

    void corruptByte(std::size_t offset, std::byte mask = std::byte{1}) {
        if (offset >= bytes_.size()) {
            throw ContractError{Errc::InvalidArgument, "corruption offset is out of range"};
        }
        bytes_[offset] ^= mask;
    }

    void clearFaults() noexcept {
        failAfterBytes_.reset();
        failBarrier_ = false;
        failResize_ = false;
    }

    [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::size_t barrierCount() const noexcept { return barrierCount_; }

private:
    template<class Transfer>
    void transfer(std::size_t size, Transfer&& transferChunk) {
        std::size_t transferred = 0;
        while (transferred != size) {
            if (failAfterBytes_ && transferred >= *failAfterBytes_) {
                failAfterBytes_.reset();
                throw DatabaseError{Errc::Io, "injected short I/O"};
            }
            auto count = std::min(maxTransferBytes_, size - transferred);
            if (failAfterBytes_) {
                count = std::min(count, *failAfterBytes_ - transferred);
            }
            if (count == 0) {
                failAfterBytes_.reset();
                throw DatabaseError{Errc::Io, "injected short I/O"};
            }
            transferChunk(transferred, count);
            transferred += count;
        }
        failAfterBytes_.reset();
    }

    std::vector<std::byte> bytes_;
    std::size_t maxTransferBytes_ = std::numeric_limits<std::size_t>::max();
    std::optional<std::size_t> failAfterBytes_;
    std::size_t barrierCount_ = 0;
    bool failBarrier_ = false;
    bool failResize_ = false;
};

} // namespace miare::testing
