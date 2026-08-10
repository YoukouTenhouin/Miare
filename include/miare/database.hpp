#pragma once

#include <miare/detail/exact_store.hpp>
#include <miare/detail/providers.hpp>
#include <miare/error.hpp>
#include <miare/result.hpp>
#include <miare/types.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <limits>
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
    using Session = detail::DatabaseSession<Allocator, Limits>;
    using OrderedKeyValues = detail::OrderedKeyValues<Allocator>;
    using StoredBytes = detail::StoredBytes<Allocator>;
    using SessionAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<Session>;

    struct ChildLifetime {
        std::atomic<bool> invalidated{false};
    };

    // allocate_shared routes the session and control block through Allocator;
    // this pointer is never copied outside the sole Database owner.
    using SessionPtr = std::shared_ptr<Session>;

public:
    using OwnedBytes = std::vector<
        std::byte,
        typename std::allocator_traits<Allocator>::template rebind_alloc<std::byte>>;

    class ReadTransaction {
    public:
        ReadTransaction(const ReadTransaction&) = delete;
        ReadTransaction& operator=(const ReadTransaction&) = delete;

        ReadTransaction(ReadTransaction&& other) noexcept
            : session_(std::exchange(other.session_, nullptr)),
              lifetime_(std::move(other.lifetime_)),
              values_(std::move(other.values_)),
              thread_(other.thread_),
              active_(std::exchange(other.active_, false)) {}

        ReadTransaction& operator=(ReadTransaction&&) = delete;

        ~ReadTransaction() { end(); }

        [[nodiscard]] std::optional<OwnedBytes> get(ByteView key) {
            requireFunctional();
            requireKey(key);
            return copyValue(*session_, values_, key);
        }

        [[nodiscard]] bool contains(ByteView key) {
            requireFunctional();
            requireKey(key);
            return values_.contains(key);
        }

        void end() noexcept {
            if (!active_) {
                return;
            }
            active_ = false;
            if (lifetime_ &&
                !lifetime_->invalidated.load(std::memory_order_acquire)) {
                releaseTransaction(*session_, *lifetime_, false);
            }
            session_ = nullptr;
            lifetime_.reset();
        }

        [[nodiscard]] bool active() const noexcept {
            return active_ && lifetime_ &&
                !lifetime_->invalidated.load(std::memory_order_acquire);
        }

    private:
        friend class Database;

        ReadTransaction(
            Session& session,
            std::shared_ptr<ChildLifetime> lifetime,
            OrderedKeyValues values)
            : session_(&session),
              lifetime_(std::move(lifetime)),
              values_(std::move(values)),
              thread_(std::this_thread::get_id()),
              active_(true) {}

        void requireFunctional() const {
            if (!active()) {
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

        Session* session_;
        std::shared_ptr<ChildLifetime> lifetime_;
        OrderedKeyValues values_;
        std::thread::id thread_;
        bool active_;
    };

    class WriteTransaction {
    public:
        WriteTransaction(const WriteTransaction&) = delete;
        WriteTransaction& operator=(const WriteTransaction&) = delete;

        WriteTransaction(WriteTransaction&& other) noexcept
            : session_(std::exchange(other.session_, nullptr)),
              lifetime_(std::move(other.lifetime_)),
              values_(std::move(other.values_)),
              thread_(other.thread_),
              keyMutations_(other.keyMutations_),
              changed_(other.changed_),
              active_(std::exchange(other.active_, false)) {}

        WriteTransaction& operator=(WriteTransaction&&) = delete;

        ~WriteTransaction() { rollback(); }

        [[nodiscard]] std::optional<OwnedBytes> get(ByteView key) {
            requireFunctional();
            requireKey(key);
            return copyValue(*session_, values_, key);
        }

        [[nodiscard]] bool contains(ByteView key) {
            requireFunctional();
            requireKey(key);
            return values_.contains(key);
        }

        void put(ByteView key, ByteView value) {
            requireFunctional();
            requireKey(key);
            if (value.size() > Limits::maxValueBytes) {
                throw ContractError{Errc::InvalidArgument, "value exceeds the capacity profile"};
            }
            requireMutationCapacity();
            using ByteAllocator = typename std::allocator_traits<Allocator>::
                template rebind_alloc<std::byte>;
            StoredBytes ownedKey{ByteAllocator{session_->allocator}};
            StoredBytes ownedValue{ByteAllocator{session_->allocator}};
            ownedKey.assign(key.begin(), key.end());
            ownedValue.assign(value.begin(), value.end());
            values_.insert_or_assign(std::move(ownedKey), std::move(ownedValue));
            ++keyMutations_;
            changed_ = true;
        }

        [[nodiscard]] bool erase(ByteView key) {
            requireFunctional();
            requireKey(key);
            const auto found = values_.find(key);
            if (found == values_.end()) {
                return false;
            }
            requireMutationCapacity();
            values_.erase(found);
            ++keyMutations_;
            changed_ = true;
            return true;
        }

        [[nodiscard]] WriteTransactionStats stats() const {
            requireFunctional();
            const auto estimatedGrowth = changed_ && !values_.empty()
                ? std::max<std::uint64_t>(
                      16U * 1024U,
                      Limits::allocationQuantumBytes)
                : 0;
            return WriteTransactionStats{
                keyMutations_,
                0,
                0,
                estimatedGrowth,
                0};
        }

        void commit() {
            requireFunctional();
            if (!changed_) {
                active_ = false;
                releaseTransaction(*session_, *lifetime_, true);
                return;
            }
            try {
                detail::commitExact<Limits>(*session_, values_);
            } catch (...) {
                if (session_->state.load(std::memory_order_acquire) ==
                    DatabaseState::RecoveryRequired) {
                    active_ = false;
                    releaseTransaction(*session_, *lifetime_, true);
                }
                throw;
            }
            active_ = false;
            releaseTransaction(*session_, *lifetime_, true);
        }

        void rollback() noexcept {
            if (!active_) {
                return;
            }
            active_ = false;
            if (lifetime_ &&
                !lifetime_->invalidated.load(std::memory_order_acquire)) {
                releaseTransaction(*session_, *lifetime_, true);
            }
            session_ = nullptr;
            lifetime_.reset();
        }

        [[nodiscard]] bool active() const noexcept {
            return active_ && lifetime_ &&
                !lifetime_->invalidated.load(std::memory_order_acquire);
        }

    private:
        friend class Database;

        WriteTransaction(
            Session& session,
            std::shared_ptr<ChildLifetime> lifetime,
            OrderedKeyValues values)
            : session_(&session),
              lifetime_(std::move(lifetime)),
              values_(std::move(values)),
              thread_(std::this_thread::get_id()),
              keyMutations_(0),
              changed_(false),
              active_(true) {}

        void requireFunctional() const {
            if (!active()) {
                throw ContractError{Errc::InvalidState, "write transaction is inactive"};
            }
            if (thread_ != std::this_thread::get_id()) {
                throw ContractError{Errc::WrongThread, "write transaction belongs to another thread"};
            }
            const auto state = session_->state.load(std::memory_order_acquire);
            if (state == DatabaseState::RecoveryRequired) {
                throw DatabaseError{Errc::RecoveryRequired, "database requires recovery"};
            }
            if (state != DatabaseState::Open) {
                throw ContractError{Errc::InvalidState, "database session is closed"};
            }
        }

        void requireMutationCapacity() const {
            if (keyMutations_ == Limits::maxKeyMutationsPerTransaction) {
                throw DatabaseError{
                    Errc::ResourceLimit,
                    "transaction key-mutation limit reached"};
            }
        }

        Session* session_;
        std::shared_ptr<ChildLifetime> lifetime_;
        OrderedKeyValues values_;
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
                    temporaryPath, key, providers, allocator);
                auto validated = requireCreatedAuthentication(
                    std::move(temporaryValidation));
                (void)validated;
            }
            detail::NativeDurableFile::installExclusive(temporaryPath, path);

            auto finalValidation = openValidated(path, key, providers, allocator);
            auto validated = requireCreatedAuthentication(
                std::move(finalValidation));
            return Database{
                std::move(validated.file),
                std::move(providers),
                std::move(allocator),
                std::move(validated.opened),
                std::move(validated.values),
                256};
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
        constexpr std::uint64_t minCacheBytes = 4ULL * 1024ULL * 1024ULL;
        constexpr std::uint64_t maxCacheBytes = 64ULL * 1024ULL * 1024ULL * 1024ULL;
        if (options.cacheCapacityBytes < minCacheBytes ||
            options.cacheCapacityBytes > maxCacheBytes ||
            options.maxReaders == 0 || options.maxReaders > 65'535) {
            throw ContractError{
                Errc::InvalidConfiguration,
                "open runtime budgets are outside their supported bounds"};
        }
        auto validation = openValidated(path, key, providers, allocator);
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
            std::move(validated.values),
            options.maxReaders});
    }

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    Database(Database&& other) noexcept
        : lifetime_(std::move(other.lifetime_)),
          session_(std::move(other.session_)) {}

    Database& operator=(Database&&) = delete;

    ~Database() {
        if (session_) {
#ifndef NDEBUG
            {
                std::lock_guard lock{session_->mutex};
                assert(session_->liveTransactions == 0 &&
                       "destroying Database with live subordinate handles");
            }
#endif
            shutdownSession(*session_, *lifetime_);
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
        if (session.activeReaders == session.maxReaders) {
            throw DatabaseError{Errc::ResourceLimit, "reader limit reached"};
        }
        auto snapshot = detail::makeOrderedKeyValues(session.allocator);
        snapshot = session.values;
        ++session.activeReaders;
        ++session.liveTransactions;
        return ReadTransaction{session, lifetime_, std::move(snapshot)};
    }

    [[nodiscard]] WriteTransaction beginWrite() {
        auto& session = requireSession();
        std::unique_lock lock{session.mutex};
        requireOpen(session);
        if (session.nextWriterTicket == std::numeric_limits<std::uint64_t>::max()) {
            throw DatabaseError{Errc::ResourceLimit, "writer admission sequence exhausted"};
        }
        const auto ticket = session.nextWriterTicket++;
        ++session.waitingWriters;
        session.writerAvailable.wait(lock, [&] {
            return (!session.writerActive &&
                    ticket == session.servingWriterTicket) ||
                session.state.load(std::memory_order_acquire) != DatabaseState::Open;
        });
        if (session.state.load(std::memory_order_acquire) != DatabaseState::Open) {
            --session.waitingWriters;
            if (ticket == session.servingWriterTicket) {
                ++session.servingWriterTicket;
            }
            session.writerAvailable.notify_all();
            requireOpen(session);
        }
        OrderedKeyValues snapshot = detail::makeOrderedKeyValues(session.allocator);
        try {
            snapshot = session.values;
        } catch (...) {
            --session.waitingWriters;
            ++session.servingWriterTicket;
            session.writerAvailable.notify_all();
            throw;
        }
        --session.waitingWriters;
        session.writerActive = true;
        ++session.liveTransactions;
        return WriteTransaction{session, lifetime_, std::move(snapshot)};
    }

    [[nodiscard]] Result<WriteTransaction, WriterBusy> tryBeginWrite() {
        auto& session = requireSession();
        std::lock_guard lock{session.mutex};
        requireOpen(session);
        if (session.writerActive || session.waitingWriters != 0) {
            return Result<WriteTransaction, WriterBusy>::failure(WriterBusy{});
        }
        if (session.nextWriterTicket == std::numeric_limits<std::uint64_t>::max()) {
            throw DatabaseError{Errc::ResourceLimit, "writer admission sequence exhausted"};
        }
        auto snapshot = detail::makeOrderedKeyValues(session.allocator);
        snapshot = session.values;
        ++session.nextWriterTicket;
        session.writerActive = true;
        ++session.liveTransactions;
        return Result<WriteTransaction, WriterBusy>::success(
            WriteTransaction{session, lifetime_, std::move(snapshot)});
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
            std::unique_lock lock{session.mutex};
            session.writerAvailable.notify_all();
            session.writerAvailable.wait(lock, [&] {
                return session.waitingWriters == 0;
            });
            session.servingWriterTicket = session.nextWriterTicket -
                static_cast<std::uint64_t>(session.writerActive);
            if (session.liveTransactions != 0) {
                auto closing = DatabaseState::Closing;
                (void)session.state.compare_exchange_strong(
                    closing,
                    expected,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire);
                throw ContractError{
                    Errc::LiveChildren,
                    "database has live transactions"};
            }
        }
        shutdownSession(session, *lifetime_);
    }

private:
    struct ValidatedFile {
        std::unique_ptr<detail::DurableFile> file;
        detail::OpenedDatabase opened;
        OrderedKeyValues values;
    };

    [[nodiscard]] static std::shared_ptr<ChildLifetime> makeChildLifetime(
        const Allocator& allocator) {
        using LifetimeAllocator = typename std::allocator_traits<Allocator>::
            template rebind_alloc<ChildLifetime>;
        return std::allocate_shared<ChildLifetime>(LifetimeAllocator{allocator});
    }

    [[nodiscard]] static SessionPtr makeSession(
        std::unique_ptr<detail::DurableFile> file,
        ProviderSet providers,
        Allocator allocator,
        detail::OpenedDatabase opened,
        OrderedKeyValues values,
        std::uint32_t maxReaders) {
        SessionAllocator sessionAllocator{allocator};
        return std::allocate_shared<Session>(
            sessionAllocator,
            std::move(file),
            std::move(providers),
            std::move(allocator),
            std::move(opened),
            std::move(values),
            maxReaders);
    }

    Database(
        std::unique_ptr<detail::DurableFile> file,
        ProviderSet providers,
        Allocator allocator,
        detail::OpenedDatabase opened,
        OrderedKeyValues values,
        std::uint32_t maxReaders)
        : lifetime_(makeChildLifetime(allocator)),
          session_(makeSession(
              std::move(file),
              std::move(providers),
              std::move(allocator),
              std::move(opened),
              std::move(values),
              maxReaders)) {}

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

    [[nodiscard]] static std::optional<OwnedBytes> copyValue(
        Session& session,
        const OrderedKeyValues& values,
        ByteView key) {
        const auto found = values.find(key);
        if (found == values.end()) {
            return std::nullopt;
        }
        using ByteAllocator = typename std::allocator_traits<Allocator>::
            template rebind_alloc<std::byte>;
        OwnedBytes result{ByteAllocator{session.allocator}};
        result.assign(found->second.begin(), found->second.end());
        return result;
    }

    static void releaseTransaction(
        Session& session,
        const ChildLifetime& lifetime,
        bool writer) noexcept {
        try {
            std::lock_guard lock{session.mutex};
            if (lifetime.invalidated.load(std::memory_order_relaxed)) {
                return;
            }
            --session.liveTransactions;
            if (writer) {
                session.writerActive = false;
                ++session.servingWriterTicket;
                session.writerAvailable.notify_all();
            } else {
                --session.activeReaders;
            }
        } catch (...) {
        }
    }

    static void eraseSessionKeys(Session& session) noexcept {
        session.opened.keys.header.erase();
        session.opened.keys.mainData.erase();
        session.opened.keys.recovery.erase();
        session.opened.keys.blob.erase();
    }

    static void shutdownSession(
        Session& session,
        ChildLifetime& lifetime) noexcept {
        try {
            std::lock_guard lock{session.mutex};
            lifetime.invalidated.store(true, std::memory_order_release);
            session.liveTransactions = 0;
            session.activeReaders = 0;
            session.waitingWriters = 0;
            session.writerActive = false;
            eraseSessionKeys(session);
            session.file.reset();
            session.providers.reset();
            session.state.store(DatabaseState::Closed, std::memory_order_release);
            session.writerAvailable.notify_all();
        } catch (...) {
            lifetime.invalidated.store(true, std::memory_order_release);
            eraseSessionKeys(session);
            session.file.reset();
            session.providers.reset();
            session.state.store(DatabaseState::Closed, std::memory_order_release);
        }
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
        ProviderSet& providers,
        const Allocator& allocator) {
        std::unique_ptr<detail::DurableFile> file =
            detail::NativeDurableFile::openExisting(path);
        auto opened = detail::openFormat<Limits>(*file, key, providers);
        if (!opened) {
            return Result<ValidatedFile, AuthenticationFailed>::failure(
                AuthenticationFailed{});
        }
        auto openedDatabase = std::move(opened).value();
        std::vector<detail::ExtentReference> reachable;
        auto values = detail::loadExactValues<Limits>(
            *file, openedDatabase, providers, allocator, &reachable);
        detail::loadAllocatorReferences<Limits>(
            *file, openedDatabase, providers, allocator, reachable);
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

    std::shared_ptr<ChildLifetime> lifetime_;
    SessionPtr session_;
};

} // namespace miare
