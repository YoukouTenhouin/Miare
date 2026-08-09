#pragma once

#include <miare/detail/database_format.hpp>
#include <miare/detail/providers.hpp>
#include <miare/error.hpp>
#include <miare/result.hpp>
#include <miare/types.hpp>

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace miare {

template<
    class Allocator = std::allocator<std::byte>,
    class Limits = DefaultLimits>
requires DatabaseAllocator<Allocator> && LimitPolicy<Limits>
class Database {
public:
    using OwnedBytes = std::vector<
        std::byte,
        typename std::allocator_traits<Allocator>::template rebind_alloc<std::byte>>;

    [[nodiscard]] static Database create(
        const std::filesystem::path& path,
        EncryptionKeyView key,
        ProviderSet providers,
        CreateOptions options = {},
        Allocator allocator = {}) {
        detail::requireCallerKey(key);
        detail::validateCreateOptions(options);
        validateTargetDoesNotExist(path);
        if (options.compression == Compression::ZStd) {
            (void)detail::ProviderAccess::compression(providers);
        }

        auto& crypto = detail::ProviderAccess::crypto(providers);
        const auto commonRegion = detail::makeInitialCommonRegion<Limits>(
            key, crypto, options.compression);
        const auto temporaryPath = createTemporaryPath(path);
        try {
            {
                auto file = detail::NativeDurableFile::createNew(temporaryPath);
                file->writeExactAt(0, commonRegion);
                file->resize(detail::commonRegionBytes);
                file->stableStorageBarrier();
            }
            {
                auto temporaryValidation = openValidated(
                    temporaryPath, key, providers);
                auto validated = requireCreatedAuthentication(
                    std::move(temporaryValidation));
                (void)validated;
            }
            detail::NativeDurableFile::installExclusive(temporaryPath, path);

            auto finalValidation = openValidated(path, key, providers);
            auto validated = requireCreatedAuthentication(
                std::move(finalValidation));
            return Database{
                std::move(validated.file),
                std::move(providers),
                std::move(allocator),
                std::move(validated.opened)};
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(temporaryPath, ignored);
            throw;
        }
    }

    [[nodiscard]] static Result<Database, AuthenticationFailed> open(
        const std::filesystem::path& path,
        EncryptionKeyView key,
        ProviderSet providers,
        OpenOptions options = {},
        Allocator allocator = {}) {
        if (options.cacheCapacityBytes == 0 || options.maxReaders == 0) {
            throw ContractError{
                Errc::InvalidConfiguration,
                "open runtime budgets must be positive"};
        }
        auto validation = openValidated(path, key, providers);
        if (!validation) {
            return Result<Database, AuthenticationFailed>::failure(
                AuthenticationFailed{});
        }
        auto validated = std::move(validation).value();
        return Result<Database, AuthenticationFailed>::success(Database{
            std::move(validated.file),
            std::move(providers),
            std::move(allocator),
            std::move(validated.opened)});
    }

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    Database(Database&& other) noexcept
        : file_(std::move(other.file_)),
          providers_(std::move(other.providers_)),
          allocator_(std::move(other.allocator_)),
          keys_(std::move(other.keys_)),
          format_(other.format_),
          state_(other.state_.load(std::memory_order_relaxed)) {
        other.state_.store(DatabaseState::Closed, std::memory_order_relaxed);
    }

    Database& operator=(Database&&) = delete;

    ~Database() {
        file_.reset();
        keys_.reset();
        providers_.reset();
        state_.store(DatabaseState::Closed, std::memory_order_relaxed);
    }

    [[nodiscard]] DatabaseState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

    void close() {
        auto expected = DatabaseState::Open;
        if (!state_.compare_exchange_strong(
                expected,
                DatabaseState::Closing,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            throw ContractError{Errc::InvalidState, "database is not open"};
        }
        file_.reset();
        keys_.reset();
        providers_.reset();
        state_.store(DatabaseState::Closed, std::memory_order_release);
    }

private:
    struct ValidatedFile {
        std::unique_ptr<detail::NativeDurableFile> file;
        detail::OpenedDatabase opened;
    };

    Database(
        std::unique_ptr<detail::NativeDurableFile> file,
        ProviderSet providers,
        Allocator allocator,
        detail::OpenedDatabase opened)
        : file_(std::move(file)),
          providers_(std::move(providers)),
          allocator_(std::move(allocator)),
          keys_(std::move(opened.keys)),
          format_(opened.format),
          state_(DatabaseState::Open) {}

    static void validateTargetDoesNotExist(const std::filesystem::path& path) {
        std::error_code error;
        const bool exists = std::filesystem::exists(path, error);
        if (error) {
            throw DatabaseError{
                Errc::Io,
                "database target inspection failed",
                error};
        }
        if (exists) {
            throw DatabaseError{Errc::Io, "database target already exists"};
        }
    }

    [[nodiscard]] static std::filesystem::path createTemporaryPath(
        const std::filesystem::path& target) {
        static std::atomic<std::uint64_t> sequence{0};
        auto parent = target.parent_path();
        if (parent.empty()) {
            parent = ".";
        }
        for (unsigned attempt = 0; attempt != 256; ++attempt) {
            auto temporaryName = target.filename();
            temporaryName += ".miare-tmp-";
            temporaryName += std::to_string(
                sequence.fetch_add(1, std::memory_order_relaxed));
            const auto candidate = parent / temporaryName;
            std::error_code error;
            if (!std::filesystem::exists(candidate, error) && !error) {
                return candidate;
            }
        }
        throw DatabaseError{Errc::Io, "could not reserve a database temporary name"};
    }

    [[nodiscard]] static Result<ValidatedFile, AuthenticationFailed> openValidated(
        const std::filesystem::path& path,
        EncryptionKeyView key,
        ProviderSet& providers) {
        auto file = detail::NativeDurableFile::openExisting(path);
        auto opened = detail::openFormat<Limits>(*file, key, providers);
        if (!opened) {
            return Result<ValidatedFile, AuthenticationFailed>::failure(
                AuthenticationFailed{});
        }
        return Result<ValidatedFile, AuthenticationFailed>::success(ValidatedFile{
            std::move(file),
            std::move(opened).value()});
    }

    [[nodiscard]] static ValidatedFile requireCreatedAuthentication(
        Result<ValidatedFile, AuthenticationFailed> validation) {
        if (!validation) {
            throw DatabaseError{
                Errc::ProviderUnavailable,
                "created database failed authentication"};
        }
        return std::move(validation).value();
    }

    std::unique_ptr<detail::NativeDurableFile> file_;
    std::optional<ProviderSet> providers_;
    Allocator allocator_;
    std::optional<detail::SessionKeys> keys_;
    detail::OpenedFormat format_{};
    std::atomic<DatabaseState> state_{DatabaseState::Closed};
};

} // namespace miare
