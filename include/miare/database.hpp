#pragma once

#include <miare/detail/ordered_cursor.hpp>
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

template<class Allocator, class Limits>
requires DatabaseAllocator<Allocator> && LimitPolicy<Limits>
class Database;

class KeyRangeView {
public:
    [[nodiscard]] static KeyRangeView all() noexcept {
        return KeyRangeView{
            detail::OrderedRangeKind::All,
            std::nullopt,
            std::nullopt};
    }

    [[nodiscard]] static KeyRangeView halfOpen(
        std::optional<ByteView> lowerInclusive,
        std::optional<ByteView> upperExclusive) noexcept {
        return KeyRangeView{
            detail::OrderedRangeKind::HalfOpen,
            lowerInclusive,
            upperExclusive};
    }

    [[nodiscard]] static KeyRangeView prefix(ByteView prefix) noexcept {
        return KeyRangeView{
            detail::OrderedRangeKind::Prefix,
            prefix,
            std::nullopt};
    }

private:
    KeyRangeView(
        detail::OrderedRangeKind kind,
        std::optional<ByteView> lower,
        std::optional<ByteView> upper) noexcept
        : kind_(kind), lower_(lower), upper_(upper) {}

    detail::OrderedRangeKind kind_;
    std::optional<ByteView> lower_;
    std::optional<ByteView> upper_;

    template<class Allocator, class Limits>
    requires DatabaseAllocator<Allocator> && LimitPolicy<Limits>
    friend class Database;
};

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
    using MutableTree = detail::MutableTreeNode<Allocator>;
    using ChildLifetime = detail::SessionChildLifetime;
    using CursorLifetime = detail::OrderedCursorLifetime<Allocator>;
    using SessionAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<Session>;

    // allocate_shared routes the session and control block through Allocator;
    // this pointer is never copied outside the sole Database owner.
    using SessionPtr = std::shared_ptr<Session>;

public:
    using OwnedBytes = std::vector<
        std::byte,
        typename std::allocator_traits<Allocator>::template rebind_alloc<std::byte>>;

public:
    using ReadCursor = detail::OrderedCursor<Allocator, Limits, false>;
    using WriteCursor = detail::OrderedCursor<Allocator, Limits, true>;

    class ReadTransaction {
    public:
        ReadTransaction(const ReadTransaction&) = delete;
        ReadTransaction& operator=(const ReadTransaction&) = delete;

        ReadTransaction(ReadTransaction&& other) noexcept
            : session_(std::exchange(other.session_, nullptr)),
              lifetime_(std::move(other.lifetime_)),
              values_(std::move(other.values_)),
              tree_(std::move(other.tree_)),
              cursorLifetime_(std::move(other.cursorLifetime_)),
              readerIdentity_(other.readerIdentity_),
              thread_(other.thread_),
              active_(std::exchange(other.active_, false)) {
            if (cursorLifetime_) {
                cursorLifetime_->root = &tree_;
            }
        }

        ReadTransaction& operator=(ReadTransaction&&) = delete;

        ~ReadTransaction() {
#ifndef NDEBUG
            assert(!active_ || !cursorLifetime_ ||
                cursorLifetime_->liveCursors == 0);
#endif
            end();
        }

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

        [[nodiscard]] ReadCursor scan(
            KeyRangeView range = KeyRangeView::all()) {
            requireFunctional();
            return makeCursor<false>(*session_, cursorLifetime_, range);
        }

        void end() noexcept {
            if (!active_) {
                return;
            }
            active_ = false;
            detail::invalidateCursors(cursorLifetime_);
            if (lifetime_ &&
                !lifetime_->invalidated.load(std::memory_order_acquire)) {
                releaseTransaction(
                    *session_, *lifetime_, false, readerIdentity_);
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
            OrderedKeyValues values,
            MutableTree tree,
            std::shared_ptr<CursorLifetime> cursorLifetime,
            std::uint64_t readerIdentity)
            : session_(&session),
              lifetime_(std::move(lifetime)),
              values_(std::move(values)),
              tree_(std::move(tree)),
              cursorLifetime_(std::move(cursorLifetime)),
              readerIdentity_(readerIdentity),
              thread_(std::this_thread::get_id()),
              active_(true) {
            cursorLifetime_->root = &tree_;
        }

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
        MutableTree tree_;
        std::shared_ptr<CursorLifetime> cursorLifetime_;
        std::uint64_t readerIdentity_;
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
              tree_(std::move(other.tree_)),
              cursorLifetime_(std::move(other.cursorLifetime_)),
              thread_(other.thread_),
              keyMutations_(other.keyMutations_),
              changed_(other.changed_),
              treeCurrent_(other.treeCurrent_),
              active_(std::exchange(other.active_, false)) {
            if (cursorLifetime_) {
                cursorLifetime_->root = &tree_;
            }
        }

        WriteTransaction& operator=(WriteTransaction&&) = delete;

        ~WriteTransaction() {
#ifndef NDEBUG
            assert(!active_ || !cursorLifetime_ ||
                cursorLifetime_->liveCursors == 0);
#endif
            rollback();
        }

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

        [[nodiscard]] WriteCursor scan(
            KeyRangeView range = KeyRangeView::all()) {
            requireFunctional();
            ensureCursorTree();
            return makeCursor<true>(*session_, cursorLifetime_, range);
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
            detail::invalidateWriteCursors(cursorLifetime_);
            treeCurrent_ = false;
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
            detail::invalidateWriteCursors(cursorLifetime_);
            treeCurrent_ = false;
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
                detail::invalidateCursors(cursorLifetime_);
                releaseTransaction(*session_, *lifetime_, true);
                return;
            }
            try {
                detail::commitExact<Limits>(*session_, values_);
            } catch (const DatabaseError& error) {
                if (error.code() == Errc::ResourceLimit) {
                    session_->capacityFailureCount.fetch_add(
                        1, std::memory_order_relaxed);
                }
                if (error.code() == Errc::Corrupt) {
                    enterRecoveryAfterCorruption(*session_, *lifetime_);
                }
                if (session_->state.load(std::memory_order_acquire) ==
                    DatabaseState::RecoveryRequired) {
                    active_ = false;
                    detail::invalidateCursors(cursorLifetime_);
                    releaseTransaction(*session_, *lifetime_, true);
                }
                throw;
            } catch (...) {
                if (session_->state.load(std::memory_order_acquire) ==
                    DatabaseState::RecoveryRequired) {
                    active_ = false;
                    detail::invalidateCursors(cursorLifetime_);
                    releaseTransaction(*session_, *lifetime_, true);
                }
                throw;
            }
            active_ = false;
            detail::invalidateCursors(cursorLifetime_);
            releaseTransaction(*session_, *lifetime_, true);
        }

        void rollback() noexcept {
            if (!active_) {
                return;
            }
            active_ = false;
            detail::invalidateCursors(cursorLifetime_);
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
            OrderedKeyValues values,
            MutableTree tree,
            std::shared_ptr<CursorLifetime> cursorLifetime)
            : session_(&session),
              lifetime_(std::move(lifetime)),
              values_(std::move(values)),
              tree_(std::move(tree)),
              cursorLifetime_(std::move(cursorLifetime)),
              thread_(std::this_thread::get_id()),
              keyMutations_(0),
              changed_(false),
              treeCurrent_(true),
              active_(true) {
            cursorLifetime_->root = &tree_;
        }

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
                session_->capacityFailureCount.fetch_add(
                    1, std::memory_order_relaxed);
                throw DatabaseError{
                    Errc::ResourceLimit,
                    "transaction key-mutation limit reached"};
            }
        }

        void ensureCursorTree() {
            if (treeCurrent_) {
                return;
            }
            auto rebuilt = detail::buildMutableTree<Limits>(
                values_, session_->allocator);
            tree_ = std::move(rebuilt);
            cursorLifetime_->root = &tree_;
            treeCurrent_ = true;
        }

        Session* session_;
        std::shared_ptr<ChildLifetime> lifetime_;
        OrderedKeyValues values_;
        MutableTree tree_;
        std::shared_ptr<CursorLifetime> cursorLifetime_;
        std::thread::id thread_;
        std::uint64_t keyMutations_;
        bool changed_;
        bool treeCurrent_;
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
                64U * 1024U * 1024U,
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
            options.cacheCapacityBytes,
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

    [[nodiscard]] DiagnosticsSnapshot diagnostics() const {
        auto& session = requireSession();
        std::lock_guard lock{session.mutex};
        const auto current = session.state.load(std::memory_order_acquire);
        if (current != DatabaseState::Open &&
            current != DatabaseState::RecoveryRequired) {
            throw ContractError{
                Errc::InvalidState,
                "database diagnostics require an open session"};
        }
        if (current == DatabaseState::Open) {
            ensureAllocatorSnapshotLoaded(session, *lifetime_);
        }
        std::optional<std::uint64_t> oldestReaderGeneration;
        std::optional<std::chrono::steady_clock::time_point> oldestReaderStart;
        for (const auto& [identity, reader] : session.activeReaders) {
            (void)identity;
            oldestReaderGeneration = std::min(
                oldestReaderGeneration.value_or(reader.generation),
                reader.generation);
            oldestReaderStart = std::min(
                oldestReaderStart.value_or(reader.startedAt),
                reader.startedAt);
        }
        const auto oldestReaderAge = oldestReaderStart
            ? std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - *oldestReaderStart)
            : std::chrono::milliseconds{0};
        std::uint64_t reclaimableBlocks = 0;
        std::uint64_t snapshotRetainedBlocks = 0;
        for (const auto& [retirementGeneration, blocks] :
             session.retiredBlocksByGeneration) {
            if (oldestReaderGeneration &&
                *oldestReaderGeneration < retirementGeneration) {
                snapshotRetainedBlocks += blocks;
            } else {
                reclaimableBlocks += blocks;
            }
        }
        const auto quantum = Limits::allocationQuantumBytes;
        DiagnosticsSnapshot result;
        result.state = current;
        result.formatVersion = detail::commonFormatVersion;
        result.capacityProfileVersion = detail::capacityProfileVersion;
        std::copy_n(
            session.opened.publication.begin() +
                detail::PublicationLayout::capacityProfileDigest,
            result.capacityProfileDigest.size(),
            result.capacityProfileDigest.begin());
        result.storageBackend = StorageBackend::BTree;
        result.compression = session.opened.format.compression;
        result.encryptionSuite = EncryptionSuite::XChaCha20Poly1305Ietf;
        result.lastCommittedGeneration = session.opened.format.generation;
        result.mainFileBytes = session.opened.format.highWaterBytes +
            session.opened.abandonedTailBytes;
        result.liveBytes = session.liveBlocks * quantum;
        result.reclaimableBytes = reclaimableBlocks * quantum;
        result.snapshotRetainedBytes = snapshotRetainedBlocks * quantum;
        result.cacheCapacityBytes = session.cacheCapacityBytes;
        result.activeReaders = static_cast<std::uint32_t>(
            session.activeReaders.size());
        result.oldestReaderGeneration = oldestReaderGeneration;
        result.oldestReaderAge = oldestReaderAge;
        result.writerActive = session.writerActive;
        result.writerQueueDepth = static_cast<std::uint32_t>(
            session.waitingWriters);
        result.recoveryRequired = current == DatabaseState::RecoveryRequired;
        result.recoveryCause = session.recoveryCause.load(
            std::memory_order_acquire);
        result.rejectedInactivePublication =
            session.opened.rejectedInactivePublication;
        result.abandonedTailBytes = session.opened.abandonedTailBytes;
        result.capacityFailureCount = session.capacityFailureCount.load(
            std::memory_order_relaxed);
        return result;
    }

    [[nodiscard]] ReadTransaction beginRead() {
        auto& session = requireSession();
        std::lock_guard lock{session.mutex};
        requireOpen(session);
        ensureValuesLoaded(session, *lifetime_);
        if (session.activeReaders.size() == session.maxReaders ||
            session.nextReaderIdentity ==
                std::numeric_limits<std::uint64_t>::max()) {
            session.capacityFailureCount.fetch_add(1, std::memory_order_relaxed);
            throw DatabaseError{Errc::ResourceLimit, "reader limit reached"};
        }
        auto snapshot = detail::makeOrderedKeyValues(session.allocator);
        snapshot = session.values;
        auto tree = snapshotCursorTree(session);
        auto cursorLifetime = detail::makeOrderedCursorLifetime(
            tree, lifetime_, session.allocator);
        const auto generation = session.opened.format.generation;
        const auto startedAt = std::chrono::steady_clock::now();
        const auto readerIdentity = session.nextReaderIdentity++;
        session.activeReaders.emplace(
            readerIdentity, detail::ActiveReader{generation, startedAt});
        ++session.liveTransactions;
        return ReadTransaction{
            session,
            lifetime_,
            std::move(snapshot),
            std::move(tree),
            std::move(cursorLifetime),
            readerIdentity};
    }

    [[nodiscard]] WriteTransaction beginWrite() {
        auto& session = requireSession();
        std::unique_lock lock{session.mutex};
        requireOpen(session);
        ensureValuesLoaded(session, *lifetime_);
        ensureAllocatorSnapshotLoaded(session, *lifetime_);
        if (session.nextWriterTicket == std::numeric_limits<std::uint64_t>::max()) {
            session.capacityFailureCount.fetch_add(1, std::memory_order_relaxed);
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
        MutableTree tree{session.allocator};
        std::shared_ptr<CursorLifetime> cursorLifetime;
        try {
            snapshot = session.values;
            tree = snapshotCursorTree(session);
            cursorLifetime = detail::makeOrderedCursorLifetime(
                tree, lifetime_, session.allocator);
        } catch (...) {
            --session.waitingWriters;
            ++session.servingWriterTicket;
            session.writerAvailable.notify_all();
            throw;
        }
        --session.waitingWriters;
        session.writerActive = true;
        ++session.liveTransactions;
        return WriteTransaction{
            session,
            lifetime_,
            std::move(snapshot),
            std::move(tree),
            std::move(cursorLifetime)};
    }

    [[nodiscard]] Result<WriteTransaction, WriterBusy> tryBeginWrite() {
        auto& session = requireSession();
        std::lock_guard lock{session.mutex};
        requireOpen(session);
        ensureValuesLoaded(session, *lifetime_);
        ensureAllocatorSnapshotLoaded(session, *lifetime_);
        if (session.writerActive || session.waitingWriters != 0) {
            return Result<WriteTransaction, WriterBusy>::failure(WriterBusy{});
        }
        if (session.nextWriterTicket == std::numeric_limits<std::uint64_t>::max()) {
            session.capacityFailureCount.fetch_add(1, std::memory_order_relaxed);
            throw DatabaseError{Errc::ResourceLimit, "writer admission sequence exhausted"};
        }
        auto snapshot = detail::makeOrderedKeyValues(session.allocator);
        snapshot = session.values;
        auto tree = snapshotCursorTree(session);
        auto cursorLifetime = detail::makeOrderedCursorLifetime(
            tree, lifetime_, session.allocator);
        ++session.nextWriterTicket;
        session.writerActive = true;
        ++session.liveTransactions;
        return Result<WriteTransaction, WriterBusy>::success(
            WriteTransaction{
                session,
                lifetime_,
                std::move(snapshot),
                std::move(tree),
                std::move(cursorLifetime)});
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

    [[nodiscard]] static MutableTree snapshotCursorTree(
        const Session& session) {
        return detail::cloneMutableTree(
            *session.cursorTree, session.allocator);
    }

    template<bool Write>
    [[nodiscard]] static detail::OrderedCursor<Allocator, Limits, Write> makeCursor(
        Session& session,
        const std::shared_ptr<CursorLifetime>& lifetime,
        KeyRangeView range) {
        return detail::makeOrderedCursor<Allocator, Limits, Write>(
            lifetime,
            range.kind_,
            range.lower_,
            range.upper_,
            session.allocator);
    }

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
        std::size_t cacheCapacityBytes,
        std::uint32_t maxReaders) {
        SessionAllocator sessionAllocator{allocator};
        return std::allocate_shared<Session>(
            sessionAllocator,
            std::move(file),
            std::move(providers),
            std::move(allocator),
            std::move(opened),
            std::move(values),
            cacheCapacityBytes,
            maxReaders);
    }

    Database(
        std::unique_ptr<detail::DurableFile> file,
        ProviderSet providers,
        Allocator allocator,
        detail::OpenedDatabase opened,
        OrderedKeyValues values,
        std::size_t cacheCapacityBytes,
        std::uint32_t maxReaders)
        : lifetime_(makeChildLifetime(allocator)),
          session_(makeSession(
              std::move(file),
              std::move(providers),
              std::move(allocator),
              std::move(opened),
              std::move(values),
              cacheCapacityBytes,
              maxReaders)) {}

    [[nodiscard]] Session& requireSession() {
        if (!session_) {
            throw ContractError{Errc::InvalidState, "database is inert"};
        }
        return *session_;
    }

    [[nodiscard]] Session& requireSession() const {
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
        bool writer,
        std::optional<std::uint64_t> readerIdentity = std::nullopt) noexcept {
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
                session.activeReaders.erase(*readerIdentity);
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

    static void enterRecoveryAfterCorruptionLocked(
        Session& session,
        ChildLifetime& lifetime) noexcept {
        session.recoveryCause.store(
            RecoveryCause::ConfirmedCorruption,
            std::memory_order_release);
        session.state.store(
            DatabaseState::RecoveryRequired,
            std::memory_order_release);
        session.liveTransactions = 0;
        session.activeReaders.clear();
        session.writerActive = false;
        lifetime.invalidated.store(true, std::memory_order_release);
        session.writerAvailable.notify_all();
    }

    static void enterRecoveryAfterCorruption(
        Session& session,
        ChildLifetime& lifetime) noexcept {
        try {
            std::lock_guard lock{session.mutex};
            enterRecoveryAfterCorruptionLocked(session, lifetime);
        } catch (...) {
            session.recoveryCause.store(
                RecoveryCause::ConfirmedCorruption,
                std::memory_order_release);
            session.state.store(
                DatabaseState::RecoveryRequired,
                std::memory_order_release);
            lifetime.invalidated.store(true, std::memory_order_release);
            session.writerAvailable.notify_all();
        }
    }

    static void ensureValuesLoaded(
        Session& session,
        ChildLifetime& lifetime) {
        if (session.valuesLoaded) {
            return;
        }
        try {
            auto values = detail::loadExactValues<Limits>(
                *session.file,
                session.opened,
                *session.providers,
                session.allocator,
                nullptr,
                session.cursorTree.get());
            session.values = std::move(values);
            session.valuesLoaded = true;
        } catch (const DatabaseError& error) {
            if (error.code() == Errc::Corrupt) {
                enterRecoveryAfterCorruptionLocked(session, lifetime);
            }
            throw;
        }
    }

    static void ensureAllocatorSnapshotLoaded(
        Session& session,
        ChildLifetime& lifetime) {
        if (session.allocatorSnapshotLoaded) {
            return;
        }
        try {
            auto snapshot = detail::loadAllocatorSnapshot<Limits>(
                *session.file,
                session.opened,
                *session.providers,
                session.allocator);
            session.liveBlocks = snapshot.liveBlocks;
            session.retiredBlocksByGeneration = std::move(
                snapshot.retiredBlocksByGeneration);
            session.allocatorSnapshotLoaded = true;
        } catch (const DatabaseError& error) {
            if (error.code() == Errc::Corrupt) {
                enterRecoveryAfterCorruptionLocked(session, lifetime);
            }
            throw;
        }
    }

    static void shutdownSession(
        Session& session,
        ChildLifetime& lifetime) noexcept {
        try {
            std::lock_guard lock{session.mutex};
            lifetime.invalidated.store(true, std::memory_order_release);
            session.liveTransactions = 0;
            session.activeReaders.clear();
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
        (void)detail::shallowValidateOrderedRoot<Limits>(
            *file, openedDatabase, providers, allocator);
        (void)detail::shallowValidateAllocatorRoot<Limits>(
            *file, openedDatabase, providers, allocator);
        auto values = detail::makeOrderedKeyValues(allocator);
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
