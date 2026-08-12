#pragma once

#include <miare/database.hpp>
#include <miare/detail/durable_file.hpp>
#include <miare/detail/providers.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <vector>

namespace miare::testing {

enum class DurableFileOperationKind {
    Read,
    Write,
    Resize,
    Barrier,
};

struct DurableFileOperation {
    DurableFileOperationKind kind;
    std::uint64_t offset;
    std::size_t requestedBytes;
    std::size_t transferredBytes;
    bool succeeded;
};

class ProviderFailureInjection {
protected:
    void failNextProviderOperation() noexcept { failProvider_ = true; }

    void failProviderIfRequested() {
        if (failProvider_) {
            failProvider_ = false;
            throw DatabaseError{Errc::ProviderUnavailable, "injected provider failure"};
        }
    }

private:
    bool failProvider_ = false;
};

class DeterministicCryptoProvider final
    : public detail::CryptoProvider,
      private ProviderFailureInjection {
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

    void hashBlake2b256(ByteView input, MutableByteView output) override {
        failProviderIfRequested();
        delegate_.hashBlake2b256(input, output);
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
    void failNextProviderOperation() noexcept {
        ProviderFailureInjection::failNextProviderOperation();
    }
    void corruptNextCiphertext() noexcept { corruptCiphertext_ = true; }

private:
    detail::SodiumCryptoProvider delegate_;
    std::uint64_t state_;
    bool failRandom_ = false;
    bool corruptCiphertext_ = false;
};

class FaultInjectingCompressionProvider final
    : public detail::CompressionProvider,
      private ProviderFailureInjection {
public:
    [[nodiscard]] std::size_t compressBound(std::size_t inputBytes) const override {
        if (requestMaximumOutputStorage_) {
            return ZSTD_compressBound(detail::maxProviderUnitBytes);
        }
        return delegate_.compressBound(inputBytes);
    }

    [[nodiscard]] std::size_t compress(
        ByteView input,
        MutableByteView output) override {
        failProviderIfRequested();
        if (reportExcessiveOutput_) {
            reportExcessiveOutput_ = false;
            return output.size() + 1;
        }
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

    void failNextProviderOperation() noexcept {
        ProviderFailureInjection::failNextProviderOperation();
    }
    void corruptNextFrame() noexcept { corruptFrame_ = true; }
    void requestMaximumOutputStorage() noexcept {
        requestMaximumOutputStorage_ = true;
    }
    void reportExcessiveOutput() noexcept { reportExcessiveOutput_ = true; }

private:
    detail::ZstdCompressionProvider delegate_;
    bool corruptFrame_ = false;
    bool requestMaximumOutputStorage_ = false;
    bool reportExcessiveOutput_ = false;
};

class MemoryDurableFile final : public detail::DurableFile {
public:
    [[nodiscard]] std::uint64_t size() const override {
        std::lock_guard lock{mutex_};
        return bytes_.size();
    }

    void readExactAt(std::uint64_t offset, MutableByteView destination) override {
        std::lock_guard lock{mutex_};
        auto& operation = beginOperation(
            DurableFileOperationKind::Read, offset, destination.size());
        if (failReadsAtOrAfter_ && offset >= *failReadsAtOrAfter_) {
            throw DatabaseError{Errc::Io, "injected positioned read failure"};
        }
        if (offset > bytes_.size() || destination.size() > bytes_.size() - offset) {
            throw DatabaseError{Errc::Io, "injected short read"};
        }
        transfer(destination.size(), [&](std::size_t position, std::size_t count) {
            std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset + position),
                        count,
                        destination.begin() + static_cast<std::ptrdiff_t>(position));
            operation.transferredBytes += count;
        });
        operation.succeeded = true;
    }

    void writeExactAt(std::uint64_t offset, ByteView source) override {
        std::lock_guard lock{mutex_};
        auto& operation = beginOperation(
            DurableFileOperationKind::Write, offset, source.size());
        if (offset > std::numeric_limits<std::size_t>::max() ||
            source.size() > std::numeric_limits<std::size_t>::max() - offset) {
            throw ContractError{Errc::InvalidArgument, "file range is not representable"};
        }
        transfer(source.size(), [&](std::size_t position, std::size_t count) {
            const auto chunkEnd = static_cast<std::size_t>(offset) + position + count;
            if (chunkEnd > bytes_.size()) {
                bytes_.resize(chunkEnd);
            }
            std::copy_n(source.begin() + static_cast<std::ptrdiff_t>(position),
                        count,
                        bytes_.begin() + static_cast<std::ptrdiff_t>(offset + position));
            unbarrieredMutations_.push_back(UnbarrieredMutation{
                MutationKind::Write,
                static_cast<std::size_t>(offset) + position,
                std::vector<std::byte>(
                    source.begin() + static_cast<std::ptrdiff_t>(position),
                    source.begin() + static_cast<std::ptrdiff_t>(position + count))});
            operation.transferredBytes += count;
        });
        operation.succeeded = true;
    }

    void resize(std::uint64_t length) override {
        std::lock_guard lock{mutex_};
        auto& operation = beginOperation(
            DurableFileOperationKind::Resize, length, 0);
        if (failResize_) {
            failResize_ = false;
            throw DatabaseError{Errc::Io, "injected resize failure"};
        }
        if (length > std::numeric_limits<std::size_t>::max()) {
            throw ContractError{Errc::InvalidArgument, "file length is not representable"};
        }
        bytes_.resize(static_cast<std::size_t>(length));
        unbarrieredMutations_.push_back(UnbarrieredMutation{
            MutationKind::Resize,
            static_cast<std::size_t>(length),
            {}});
        operation.succeeded = true;
    }

    void stableStorageBarrier() override {
        std::lock_guard lock{mutex_};
        auto& operation = beginOperation(DurableFileOperationKind::Barrier, 0, 0);
        if (failBarrier_ || (failBarrierAfter_ && *failBarrierAfter_ == 0)) {
            failBarrier_ = false;
            failBarrierAfter_.reset();
            throw DatabaseError{Errc::Durability, "injected barrier failure"};
        }
        if (failBarrierAfter_) {
            --*failBarrierAfter_;
        }
        stableBytes_ = bytes_;
        unbarrieredMutations_.clear();
        ++barrierCount_;
        operation.succeeded = true;
    }

    void setMaxTransferBytes(std::size_t bytes) {
        if (bytes == 0) {
            throw ContractError{Errc::InvalidArgument, "transfer size must be positive"};
        }
        maxTransferBytes_ = bytes;
    }

    void failAfterTransferredBytes(std::size_t bytes) { failAfterBytes_ = bytes; }
    void failReadsAtOrAfter(std::uint64_t offset) noexcept {
        failReadsAtOrAfter_ = offset;
    }
    void failNextBarrier() noexcept { failBarrier_ = true; }
    void failBarrierAfter(std::size_t successfulBarriers) noexcept {
        failBarrierAfter_ = successfulBarriers;
    }
    void failNextResize() noexcept { failResize_ = true; }

    void corruptByte(std::size_t offset, std::byte mask = std::byte{1}) {
        if (offset >= bytes_.size()) {
            throw ContractError{Errc::InvalidArgument, "corruption offset is out of range"};
        }
        bytes_[offset] ^= mask;
        if (offset < stableBytes_.size()) {
            stableBytes_[offset] ^= mask;
        }
    }

    void simulateCrash(std::span<const std::size_t> retainedMutationOrder = {}) {
        bytes_ = stableBytes_;
        for (const auto mutationIndex : retainedMutationOrder) {
            if (mutationIndex >= unbarrieredMutations_.size()) {
                throw ContractError{
                    Errc::InvalidArgument,
                    "retained crash mutation is out of bounds"};
            }
            const auto& mutation = unbarrieredMutations_[mutationIndex];
            if (mutation.kind == MutationKind::Resize) {
                bytes_.resize(mutation.offsetOrLength);
                continue;
            }
            const auto end = mutation.offsetOrLength + mutation.bytes.size();
            if (end > bytes_.size()) bytes_.resize(end);
            std::copy_n(
                mutation.bytes.begin(),
                mutation.bytes.size(),
                bytes_.begin() + static_cast<std::ptrdiff_t>(mutation.offsetOrLength));
        }
        stableBytes_ = bytes_;
        unbarrieredMutations_.clear();
    }

    void clearFaults() noexcept {
        failAfterBytes_.reset();
        failReadsAtOrAfter_.reset();
        failBarrier_ = false;
        failBarrierAfter_.reset();
        failResize_ = false;
    }

    [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept { return bytes_; }
    void replaceStableBytes(ByteView bytes) {
        bytes_.assign(bytes.begin(), bytes.end());
        stableBytes_ = bytes_;
        unbarrieredMutations_.clear();
    }
    [[nodiscard]] std::size_t barrierCount() const noexcept { return barrierCount_; }
    [[nodiscard]] const std::vector<DurableFileOperation>& operations() const noexcept {
        return operations_;
    }
    void clearOperations() noexcept { operations_.clear(); }
    [[nodiscard]] std::size_t unbarrieredMutationCount() const noexcept {
        return unbarrieredMutations_.size();
    }

private:
    enum class MutationKind { Write, Resize };

    struct UnbarrieredMutation {
        MutationKind kind;
        std::size_t offsetOrLength;
        std::vector<std::byte> bytes;
    };

    DurableFileOperation& beginOperation(
        DurableFileOperationKind kind,
        std::uint64_t offset,
        std::size_t requestedBytes) {
        return operations_.emplace_back(
            DurableFileOperation{kind, offset, requestedBytes, 0, false});
    }

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
    std::vector<std::byte> stableBytes_;
    std::vector<DurableFileOperation> operations_;
    std::vector<UnbarrieredMutation> unbarrieredMutations_;
    std::size_t maxTransferBytes_ = std::numeric_limits<std::size_t>::max();
    std::optional<std::size_t> failAfterBytes_;
    std::optional<std::uint64_t> failReadsAtOrAfter_;
    std::size_t barrierCount_ = 0;
    bool failBarrier_ = false;
    std::optional<std::size_t> failBarrierAfter_;
    bool failResize_ = false;
    mutable std::mutex mutex_;
};

class DatabaseAccess {
public:
    template<class Allocator, class Limits>
    [[nodiscard]] static auto lockSession(
        Database<Allocator, Limits>& database) {
        return std::unique_lock{database.session_->mutex};
    }

    template<class Allocator, class Limits>
    static void forceRecoveryRequired(Database<Allocator, Limits>& database) {
        database.session_->state.store(
            DatabaseState::RecoveryRequired, std::memory_order_release);
    }

    template<class Allocator, class Limits>
    [[nodiscard]] static std::size_t waitingWriters(
        Database<Allocator, Limits>& database) {
        std::lock_guard lock{database.session_->mutex};
        return database.session_->waitingWriters;
    }

    template<class Allocator, class Limits>
    [[nodiscard]] static bool sessionKeysErased(
        Database<Allocator, Limits>& database) {
        const auto erased = [](const detail::Secret32& secret) {
            return std::all_of(
                secret.view().begin(),
                secret.view().end(),
                [](std::byte byte) { return byte == std::byte{0}; });
        };
        const auto& keys = database.session_->opened.keys;
        return erased(keys.header) && erased(keys.mainData) &&
            erased(keys.recovery) && erased(keys.blob);
    }

    template<class Allocator = std::allocator<std::byte>, class Limits = DefaultLimits>
    [[nodiscard]] static Database<Allocator, Limits> create(
        std::unique_ptr<MemoryDurableFile> file,
        EncryptionKeyView key,
        ProviderSet providers,
        CreateOptions options = {},
        Allocator allocator = {}) {
        detail::validateCreateOptions(options);
        auto& crypto = detail::ProviderAccess::crypto(providers);
        const auto commonRegion = detail::makeInitialCommonRegion<Limits>(
            key, crypto, options.compression);
        file->writeExactAt(0, commonRegion);
        file->resize(detail::commonRegionBytes);
        file->stableStorageBarrier();
        auto opened = open<Allocator, Limits>(
            std::move(file),
            key,
            std::move(providers),
            std::move(allocator));
        if (!opened) {
            throw DatabaseError{
                Errc::ProviderUnavailable,
                "in-memory database failed authentication"};
        }
        return std::move(opened).value();
    }

    template<class Allocator = std::allocator<std::byte>, class Limits = DefaultLimits>
    [[nodiscard]] static Result<Database<Allocator, Limits>, AuthenticationFailed> open(
        std::unique_ptr<MemoryDurableFile> file,
        EncryptionKeyView key,
        ProviderSet providers,
        Allocator allocator = {}) {
        auto opened = detail::openFormat<Limits>(*file, key, providers);
        if (!opened) {
            return Result<Database<Allocator, Limits>, AuthenticationFailed>::failure(
                AuthenticationFailed{});
        }
        auto openedDatabase = std::move(opened).value();
        (void)detail::shallowValidateOrderedRoot<Limits>(
            *file, openedDatabase, providers, allocator);
        (void)detail::loadBlobCatalog<Limits>(
            *file, openedDatabase, providers, allocator);
        (void)detail::shallowValidateAllocatorRoot<Limits>(
            *file, openedDatabase, providers, allocator);
        auto values = detail::makeOrderedKeyValues(allocator);
        std::unique_ptr<detail::DurableFile> durableFile = std::move(file);
        return Result<Database<Allocator, Limits>, AuthenticationFailed>::success(
            Database<Allocator, Limits>{
                std::move(durableFile),
                std::move(providers),
                std::move(allocator),
                std::move(openedDatabase),
                std::move(values),
                64U * 1024U * 1024U,
                256});
    }
};

} // namespace miare::testing
