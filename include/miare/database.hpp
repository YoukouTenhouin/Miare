#pragma once

#include <miare/detail/exact_store.hpp>
#include <miare/detail/providers.hpp>
#include <miare/error.hpp>
#include <miare/result.hpp>
#include <miare/types.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace miare {

namespace testing {
class DatabaseAccess;
}

template<
    class Allocator = std::allocator<std::byte>,
    class Limits = DefaultLimits>
requires DatabaseAllocator<Allocator> && LimitPolicy<Limits>
class Database {
private:
    friend class testing::DatabaseAccess;

    struct Session {
        Session(
            std::unique_ptr<detail::DurableFile> openedFile,
            ProviderSet openedProviders,
            Allocator openedAllocator,
            detail::OpenedDatabase openedDatabase,
            detail::ExactValues openedValues)
            : file(std::move(openedFile)),
              providers(std::move(openedProviders)),
              allocator(std::move(openedAllocator)),
              opened(std::move(openedDatabase)),
              values(std::move(openedValues)) {}

        std::unique_ptr<detail::DurableFile> file;
        std::optional<ProviderSet> providers;
        Allocator allocator;
        detail::OpenedDatabase opened;
        detail::ExactValues values;
        std::mutex mutex;
        std::condition_variable writerAvailable;
        bool writerActive = false;
        std::size_t liveTransactions = 0;
        std::atomic<DatabaseState> state{DatabaseState::Open};
    };

public:
    using OwnedBytes = std::vector<
        std::byte,
        typename std::allocator_traits<Allocator>::template rebind_alloc<std::byte>>;

    class ReadTransaction {
    public:
        ReadTransaction(const ReadTransaction&) = delete;
        ReadTransaction& operator=(const ReadTransaction&) = delete;

        ReadTransaction(ReadTransaction&& other) noexcept
            : session_(std::move(other.session_)),
              values_(std::move(other.values_)),
              thread_(other.thread_),
              active_(std::exchange(other.active_, false)) {
            other.session_.reset();
        }

        ReadTransaction& operator=(ReadTransaction&&) = delete;

        ~ReadTransaction() { end(); }

        [[nodiscard]] std::optional<OwnedBytes> get(ByteView key) {
            requireFunctional();
            requireKey(key);
            const auto found = values_.find(detail::ExactBytes{key.begin(), key.end()});
            if (found == values_.end()) {
                return std::nullopt;
            }
            using ByteAllocator = typename std::allocator_traits<Allocator>::
                template rebind_alloc<std::byte>;
            OwnedBytes result{ByteAllocator{session_->allocator}};
            result.assign(found->second.begin(), found->second.end());
            return result;
        }

        [[nodiscard]] bool contains(ByteView key) {
            requireFunctional();
            requireKey(key);
            return values_.contains(detail::ExactBytes{key.begin(), key.end()});
        }

        void end() noexcept {
            if (!active_) {
                return;
            }
            active_ = false;
            releaseTransaction(*session_, false);
        }

        [[nodiscard]] bool active() const noexcept { return active_; }

    private:
        friend class Database;

        ReadTransaction(
            std::shared_ptr<Session> session,
            detail::ExactValues values)
            : session_(std::move(session)),
              values_(std::move(values)),
              thread_(std::this_thread::get_id()),
              active_(true) {}

        void requireFunctional() const {
            if (!active_) {
                throw ContractError{Errc::InvalidState, "read transaction is inactive"};
            }
            if (thread_ != std::this_thread::get_id()) {
                throw ContractError{Errc::WrongThread, "read transaction belongs to another thread"};
            }
            const auto state = session_->state.load(std::memory_order_acquire);
            if (state == DatabaseState::Closed || state == DatabaseState::Closing) {
                throw ContractError{Errc::InvalidState, "database session is closed"};
            }
        }

        std::shared_ptr<Session> session_;
        detail::ExactValues values_;
        std::thread::id thread_;
        bool active_;
    };

    class WriteTransaction {
    public:
        WriteTransaction(const WriteTransaction&) = delete;
        WriteTransaction& operator=(const WriteTransaction&) = delete;

        WriteTransaction(WriteTransaction&& other) noexcept
            : session_(std::move(other.session_)),
              values_(std::move(other.values_)),
              thread_(other.thread_),
              keyMutations_(other.keyMutations_),
              changed_(other.changed_),
              active_(std::exchange(other.active_, false)) {
            other.session_.reset();
        }

        WriteTransaction& operator=(WriteTransaction&&) = delete;

        ~WriteTransaction() { rollback(); }

        [[nodiscard]] std::optional<OwnedBytes> get(ByteView key) {
            requireFunctional();
            requireKey(key);
            const auto found = values_.find(detail::ExactBytes{key.begin(), key.end()});
            if (found == values_.end()) {
                return std::nullopt;
            }
            using ByteAllocator = typename std::allocator_traits<Allocator>::
                template rebind_alloc<std::byte>;
            OwnedBytes result{ByteAllocator{session_->allocator}};
            result.assign(found->second.begin(), found->second.end());
            return result;
        }

        [[nodiscard]] bool contains(ByteView key) {
            requireFunctional();
            requireKey(key);
            return values_.contains(detail::ExactBytes{key.begin(), key.end()});
        }

        void put(ByteView key, ByteView value) {
            requireFunctional();
            requireKey(key);
            if (value.size() > Limits::maxValueBytes) {
                throw ContractError{Errc::InvalidArgument, "value exceeds the capacity profile"};
            }
            requireMutationCapacity();
            detail::ExactBytes ownedKey{key.begin(), key.end()};
            detail::ExactBytes ownedValue{value.begin(), value.end()};
            values_.insert_or_assign(std::move(ownedKey), std::move(ownedValue));
            ++keyMutations_;
            changed_ = true;
        }

        [[nodiscard]] bool erase(ByteView key) {
            requireFunctional();
            requireKey(key);
            const detail::ExactBytes ownedKey{key.begin(), key.end()};
            if (!values_.contains(ownedKey)) {
                return false;
            }
            requireMutationCapacity();
            values_.erase(ownedKey);
            ++keyMutations_;
            changed_ = true;
            return true;
        }

        [[nodiscard]] WriteTransactionStats stats() const {
            requireFunctional();
            return WriteTransactionStats{
                keyMutations_,
                0,
                0,
                0,
                0};
        }

        void commit() {
            requireFunctional();
            if (!changed_) {
                active_ = false;
                releaseTransaction(*session_, true);
                return;
            }
            try {
                commitExact(*session_, values_);
            } catch (...) {
                if (session_->state.load(std::memory_order_acquire) ==
                    DatabaseState::RecoveryRequired) {
                    active_ = false;
                    releaseTransaction(*session_, true);
                }
                throw;
            }
            active_ = false;
            releaseTransaction(*session_, true);
        }

        void rollback() noexcept {
            if (!active_) {
                return;
            }
            active_ = false;
            releaseTransaction(*session_, true);
        }

        [[nodiscard]] bool active() const noexcept { return active_; }

    private:
        friend class Database;

        WriteTransaction(
            std::shared_ptr<Session> session,
            detail::ExactValues values)
            : session_(std::move(session)),
              values_(std::move(values)),
              thread_(std::this_thread::get_id()),
              keyMutations_(0),
              changed_(false),
              active_(true) {}

        void requireFunctional() const {
            if (!active_) {
                throw ContractError{Errc::InvalidState, "write transaction is inactive"};
            }
            if (thread_ != std::this_thread::get_id()) {
                throw ContractError{Errc::WrongThread, "write transaction belongs to another thread"};
            }
            if (session_->state.load(std::memory_order_acquire) != DatabaseState::Open) {
                throw DatabaseError{Errc::RecoveryRequired, "database requires recovery"};
            }
        }

        void requireMutationCapacity() const {
            if (keyMutations_ == Limits::maxKeyMutationsPerTransaction) {
                throw DatabaseError{
                    Errc::ResourceLimit,
                    "transaction key-mutation limit reached"};
            }
        }

        std::shared_ptr<Session> session_;
        detail::ExactValues values_;
        std::thread::id thread_;
        std::uint64_t keyMutations_;
        bool changed_;
        bool active_;
    };

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
                std::move(validated.opened),
                std::move(validated.values)};
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
            std::move(validated.opened),
            std::move(validated.values)});
    }

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    Database(Database&& other) noexcept : session_(std::move(other.session_)) {}

    Database& operator=(Database&&) = delete;

    ~Database() {
        if (session_) {
            session_->file.reset();
            session_->providers.reset();
            session_->state.store(DatabaseState::Closed, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] DatabaseState state() const noexcept {
        return session_
            ? session_->state.load(std::memory_order_acquire)
            : DatabaseState::Closed;
    }

    [[nodiscard]] ReadTransaction beginRead() {
        auto& session = requireSession();
        std::lock_guard lock{session.mutex};
        requireOpen(session);
        auto snapshot = session.values;
        ++session.liveTransactions;
        return ReadTransaction{session_, std::move(snapshot)};
    }

    [[nodiscard]] WriteTransaction beginWrite() {
        auto& session = requireSession();
        std::unique_lock lock{session.mutex};
        requireOpen(session);
        session.writerAvailable.wait(lock, [&] {
            return !session.writerActive ||
                session.state.load(std::memory_order_acquire) != DatabaseState::Open;
        });
        requireOpen(session);
        auto snapshot = session.values;
        session.writerActive = true;
        ++session.liveTransactions;
        return WriteTransaction{session_, std::move(snapshot)};
    }

    [[nodiscard]] Result<WriteTransaction, WriterBusy> tryBeginWrite() {
        auto& session = requireSession();
        std::lock_guard lock{session.mutex};
        requireOpen(session);
        if (session.writerActive) {
            return Result<WriteTransaction, WriterBusy>::failure(WriterBusy{});
        }
        auto snapshot = session.values;
        session.writerActive = true;
        ++session.liveTransactions;
        return Result<WriteTransaction, WriterBusy>::success(
            WriteTransaction{session_, std::move(snapshot)});
    }

    void close() {
        auto& session = requireSession();
        auto expected = session.state.load(std::memory_order_acquire);
        for (;;) {
            if (expected != DatabaseState::Open &&
                expected != DatabaseState::RecoveryRequired) {
                throw ContractError{Errc::InvalidState, "database is not open"};
            }
            if (session.state.compare_exchange_weak(
                    expected,
                    DatabaseState::Closing,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                break;
            }
        }
        {
            std::lock_guard lock{session.mutex};
            if (session.liveTransactions != 0) {
                session.state.store(expected, std::memory_order_release);
                throw ContractError{
                    Errc::LiveChildren,
                    "database has live transactions"};
            }
        }
        session.file.reset();
        session.providers.reset();
        session.state.store(DatabaseState::Closed, std::memory_order_release);
    }

private:
    struct ValidatedFile {
        std::unique_ptr<detail::DurableFile> file;
        detail::OpenedDatabase opened;
        detail::ExactValues values;
    };

    Database(
        std::unique_ptr<detail::DurableFile> file,
        ProviderSet providers,
        Allocator allocator,
        detail::OpenedDatabase opened,
        detail::ExactValues values)
        : session_(std::make_shared<Session>(
              std::move(file),
              std::move(providers),
              std::move(allocator),
              std::move(opened),
              std::move(values))) {}

    [[nodiscard]] Session& requireSession() {
        if (!session_) {
            throw ContractError{Errc::InvalidState, "database is inert"};
        }
        return *session_;
    }

    static void requireOpen(const Session& session) {
        const auto current = session.state.load(std::memory_order_acquire);
        if (current == DatabaseState::RecoveryRequired) {
            throw DatabaseError{Errc::RecoveryRequired, "database requires recovery"};
        }
        if (current != DatabaseState::Open) {
            throw ContractError{Errc::InvalidState, "database is not open"};
        }
    }

    static void requireKey(ByteView key) {
        if (key.size() > Limits::maxKeyBytes) {
            throw ContractError{Errc::InvalidArgument, "key exceeds the capacity profile"};
        }
    }

    static void releaseTransaction(Session& session, bool writer) noexcept {
        try {
            std::lock_guard lock{session.mutex};
            --session.liveTransactions;
            if (writer) {
                session.writerActive = false;
                session.writerAvailable.notify_one();
            }
        } catch (...) {
        }
    }

    static void commitExact(Session& session, const detail::ExactValues& values) {
        const auto generation = session.opened.format.generation + 1;
        if (generation == 0) {
            throw DatabaseError{Errc::ResourceLimit, "database generation is exhausted"};
        }
        const auto startBlock =
            session.opened.format.highWaterBytes / Limits::allocationQuantumBytes;
        std::optional<detail::PreparedExactExtent> leaf;
        detail::ExtentReference root;
        std::uint64_t highWaterBytes = session.opened.format.highWaterBytes;
        if (!values.empty()) {
            leaf = detail::prepareLeafExtent<Limits>(
                values,
                generation,
                startBlock,
                session.opened,
                *session.providers);
            root = leaf->reference;
            highWaterBytes += leaf->bytes.size();
        }
        auto publication = detail::prepareExactPublication<Limits>(
            session.opened,
            root,
            highWaterBytes,
            *session.providers);

        bool publicationStarted = false;
        try {
            if (leaf) {
                session.file->writeExactAt(
                    root.blockIndex * Limits::allocationQuantumBytes,
                    leaf->bytes);
            }
            session.file->stableStorageBarrier();
            publicationStarted = true;
            session.file->writeExactAt(
                detail::bootstrapBytes +
                    publication.slotIndex * detail::publicationSlotBytes,
                publication.slot);
            session.file->stableStorageBarrier();
        } catch (const DatabaseError& error) {
            session.state.store(
                DatabaseState::RecoveryRequired, std::memory_order_release);
            throw DatabaseError{
                publicationStarted
                    ? Errc::CommitOutcomeUnknown
                    : Errc::CommitFailed,
                publicationStarted
                    ? "commit publication outcome is unknown"
                    : "commit failed before publication",
                error.nativeCode()};
        } catch (...) {
            session.state.store(
                DatabaseState::RecoveryRequired, std::memory_order_release);
            throw;
        }

        session.opened.publication = std::move(publication.plaintext);
        session.opened.format.generation = generation;
        session.opened.format.highWaterBytes = highWaterBytes;
        session.opened.format.orderedRoot = detail::encodeExtentReference(root);
        session.values = values;
    }

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
        std::unique_ptr<detail::DurableFile> file =
            detail::NativeDurableFile::openExisting(path);
        auto opened = detail::openFormat<Limits>(*file, key, providers);
        if (!opened) {
            return Result<ValidatedFile, AuthenticationFailed>::failure(
                AuthenticationFailed{});
        }
        auto openedDatabase = std::move(opened).value();
        auto values = detail::loadExactValues<Limits>(
            *file, openedDatabase, providers);
        return Result<ValidatedFile, AuthenticationFailed>::success(ValidatedFile{
            std::move(file),
            std::move(openedDatabase),
            std::move(values)});
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

    std::shared_ptr<Session> session_;
};

} // namespace miare
