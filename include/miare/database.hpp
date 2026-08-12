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
#include <shared_mutex>
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

    using BlobVersion = detail::BlobVersion<Allocator>;
    using BlobVersionPtr = detail::BlobVersionPtr<Allocator>;
    using BlobCatalog = detail::BlobCatalog<Allocator>;
    using BlobReaderLifetime = detail::BlobReaderLifetime;
    using WriteBlobState = detail::BlobWriteState<Allocator, Limits>;

    [[nodiscard]] static BlobCatalog makeBlobCatalog(const Allocator& allocator) {
        return detail::makeBlobCatalog(allocator);
    }

    [[nodiscard]] static std::shared_ptr<BlobReaderLifetime>
    makeBlobReaderLifetime(const Allocator& allocator) {
        using LifetimeAllocator = typename std::allocator_traits<Allocator>::
            template rebind_alloc<BlobReaderLifetime>;
        return std::allocate_shared<BlobReaderLifetime>(
            LifetimeAllocator{allocator});
    }

    [[nodiscard]] static std::shared_ptr<WriteBlobState>
    makeWriteBlobState(Session& session) {
        using StateAllocator = typename std::allocator_traits<Allocator>::
            template rebind_alloc<WriteBlobState>;
        return std::allocate_shared<WriteBlobState>(
            StateAllocator{session.allocator}, session);
    }

public:
    using OwnedBytes = std::vector<
        std::byte,
        typename std::allocator_traits<Allocator>::template rebind_alloc<std::byte>>;

public:
    using ReadCursor = detail::OrderedCursor<Allocator, Limits, false>;
    using WriteCursor = detail::OrderedCursor<Allocator, Limits, true>;

    class BlobReader {
    public:
        BlobReader(const BlobReader&) = delete;
        BlobReader& operator=(const BlobReader&) = delete;

        BlobReader(BlobReader&& other) noexcept
            : session_(std::exchange(other.session_, nullptr)),
              sessionLifetime_(std::move(other.sessionLifetime_)),
              lifetime_(std::move(other.lifetime_)),
              version_(std::move(other.version_)),
              cacheState_(std::move(other.cacheState_)),
              decodedChunk_(std::move(other.decodedChunk_)),
              thread_(other.thread_),
              position_(other.position_),
              active_(std::exchange(other.active_, false)) {}

        BlobReader& operator=(BlobReader&&) = delete;

        ~BlobReader() { close(); }

        [[nodiscard]] BlobId id() const {
            requireFunctional();
            return version_->id;
        }

        [[nodiscard]] std::uint64_t size() const {
            requireFunctional();
            return version_->size;
        }

        [[nodiscard]] std::uint64_t position() const {
            requireFunctional();
            return position_;
        }

        std::size_t read(MutableByteView destination) {
            requireFunctional();
            try {
                std::shared_lock operation{session_->operationMutex};
                requireFunctional();
                const auto available = version_->size - position_;
                const auto count = static_cast<std::size_t>(
                    std::min<std::uint64_t>(available, destination.size()));
                if (count != 0) {
                    detail::readBlobRange<Limits>(
                        *session_,
                        *version_,
                        position_,
                        destination.first(count),
                        decodedChunk_);
                    position_ += count;
                }
                return count;
            } catch (const DatabaseError& error) {
                if (error.code() == Errc::Corrupt) {
                    Database::enterRecoveryAfterCorruption(
                        *session_, *sessionLifetime_);
                }
                throw;
            }
        }

        void seek(std::uint64_t absoluteOffset) {
            requireFunctional();
            if (absoluteOffset > version_->size) {
                throw ContractError{
                    Errc::InvalidArgument,
                    "Blob seek exceeds its logical size"};
            }
            position_ = absoluteOffset;
        }

        void close() noexcept {
            if (!active_) {
                return;
            }
            active_ = false;
            if (lifetime_ &&
                !lifetime_->invalidated.load(std::memory_order_acquire)) {
                lifetime_->liveReaders.fetch_sub(1, std::memory_order_relaxed);
            }
            if (cacheState_) {
                detail::releaseDecodedBlobChunk(
                    *cacheState_, decodedChunk_);
            }
            version_.reset();
            lifetime_.reset();
            sessionLifetime_.reset();
            cacheState_.reset();
            session_ = nullptr;
        }

        [[nodiscard]] bool active() const noexcept {
            return active_ && lifetime_ && sessionLifetime_ &&
                !lifetime_->invalidated.load(std::memory_order_acquire) &&
                !sessionLifetime_->invalidated.load(std::memory_order_acquire);
        }

    private:
        friend class Database;
        friend class ReadTransaction;
        friend class WriteTransaction;

        BlobReader(
            Session& session,
            std::shared_ptr<ChildLifetime> sessionLifetime,
            std::shared_ptr<BlobReaderLifetime> lifetime,
            BlobVersionPtr version,
            std::thread::id thread)
            : session_(&session),
              sessionLifetime_(std::move(sessionLifetime)),
              lifetime_(std::move(lifetime)),
              version_(std::move(version)),
              cacheState_(session.blobCache),
              thread_(thread),
              position_(0),
              active_(true) {
            lifetime_->liveReaders.fetch_add(1, std::memory_order_relaxed);
        }

        void requireFunctional() const {
            if (!active()) {
                throw ContractError{Errc::InvalidState, "Blob reader is inactive"};
            }
            if (thread_ != std::this_thread::get_id()) {
                throw ContractError{
                    Errc::WrongThread,
                    "Blob reader belongs to another thread"};
            }
            if (session_->confirmedCorruptionPending.load(
                    std::memory_order_acquire)) {
                throw DatabaseError{
                    Errc::RecoveryRequired,
                    "database requires recovery"};
            }
        }

        Session* session_;
        std::shared_ptr<ChildLifetime> sessionLifetime_;
        std::shared_ptr<BlobReaderLifetime> lifetime_;
        BlobVersionPtr version_;
        std::shared_ptr<detail::BlobCacheState> cacheState_;
        std::optional<detail::DecodedBlobChunk<Allocator>> decodedChunk_;
        std::thread::id thread_;
        std::uint64_t position_;
        bool active_;
    };

private:
    [[nodiscard]] static BlobReader makeBlobReader(
        Session& session,
        const std::shared_ptr<ChildLifetime>& sessionLifetime,
        const std::shared_ptr<BlobReaderLifetime>& readerLifetime,
        const BlobVersionPtr& version,
        std::thread::id thread) {
        if (readerLifetime->liveReaders.load(std::memory_order_relaxed) ==
            Limits::maxBlobReadersPerTransaction) {
            session.capacityFailureCount.fetch_add(
                1, std::memory_order_relaxed);
            throw DatabaseError{
                Errc::ResourceLimit,
                "transaction Blob-reader limit reached"};
        }
        return BlobReader{
            session, sessionLifetime, readerLifetime, version, thread};
    }

public:

    class BlobWriter {
    public:
        BlobWriter(const BlobWriter&) = delete;
        BlobWriter& operator=(const BlobWriter&) = delete;

        BlobWriter(BlobWriter&& other) noexcept
            : state_(std::move(other.state_)),
              sessionLifetime_(std::move(other.sessionLifetime_)),
              buffer_(std::move(other.buffer_)),
              stagedChunks_(std::move(other.stagedChunks_)),
              id_(other.id_),
              position_(other.position_),
              active_(std::exchange(other.active_, false)) {}

        BlobWriter& operator=(BlobWriter&&) = delete;

        ~BlobWriter() { abort(); }

        [[nodiscard]] BlobId id() const {
            requireFunctional();
            return id_;
        }

        [[nodiscard]] std::uint64_t position() const {
            requireFunctional();
            return position_;
        }

        void write(ByteView source) {
            requireFunctional();
            using ByteAllocator = typename std::allocator_traits<Allocator>::
                template rebind_alloc<std::byte>;
            using ReferenceAllocator = typename std::allocator_traits<Allocator>::
                template rebind_alloc<detail::ExtentReference>;
            detail::ExtentReferences<Allocator> prepared{
                ReferenceAllocator{state_->session->allocator}};
            try {
                std::shared_lock operation{
                    state_->session->operationMutex};
                requireFunctional();
                if (source.size() > Limits::maxBlobBytes - position_ ||
                    source.size() > Limits::maxBlobBytesPerTransaction -
                        state_->bytesWritten) {
                    state_->session->capacityFailureCount.fetch_add(
                        1, std::memory_order_relaxed);
                    throw DatabaseError{
                        Errc::ResourceLimit,
                        "Blob streaming exceeds the capacity profile"};
                }
                StoredBytes working{
                    ByteAllocator{state_->session->allocator}};
                working = buffer_;
                const auto possibleChunks =
                    source.size() / Limits::blobChunkBytes +
                    (source.size() % Limits::blobChunkBytes + working.size()) /
                        Limits::blobChunkBytes;
                stagedChunks_.reserve(stagedChunks_.size() + possibleChunks);
                prepared.reserve(possibleChunks);
                if (possibleChunks != 0) {
                    detail::initializeBlobStagingAllocator<Limits>(*state_);
                    state_->stagingFreeRuns.reserve(
                        state_->stagingFreeRuns.size() + possibleChunks);
                }
                std::size_t consumed = 0;
                while (consumed != source.size()) {
                    const auto count = std::min<std::size_t>(
                        Limits::blobChunkBytes - working.size(),
                        source.size() - consumed);
                    working.insert(
                        working.end(),
                        source.begin() + static_cast<std::ptrdiff_t>(consumed),
                        source.begin() +
                            static_cast<std::ptrdiff_t>(consumed + count));
                    consumed += count;
                    if (working.size() == Limits::blobChunkBytes) {
                        prepared.push_back(detail::stageBlobChunk<Limits>(
                            *state_,
                            id_,
                            stagedChunks_.size() + prepared.size(),
                            working));
                        working.clear();
                    }
                }
                stagedChunks_.insert(
                    stagedChunks_.end(), prepared.begin(), prepared.end());
                buffer_ = std::move(working);
                position_ += source.size();
                state_->bytesWritten += source.size();
            } catch (const DatabaseError& error) {
                detail::releaseBlobStagingReferences<Limits>(
                    *state_, prepared);
                handleCorruption(error);
                throw;
            } catch (...) {
                detail::releaseBlobStagingReferences<Limits>(
                    *state_, prepared);
                throw;
            }
        }

        void finish() {
            requireFunctional();
            using ReferenceAllocator = typename std::allocator_traits<Allocator>::
                template rebind_alloc<detail::ExtentReference>;
            using VersionAllocator = typename std::allocator_traits<Allocator>::
                template rebind_alloc<BlobVersion>;
            std::optional<detail::ExtentReference> finalChunk;
            try {
                std::shared_lock operation{
                    state_->session->operationMutex};
                requireFunctional();
                detail::ExtentReferences<Allocator> chunks{
                    stagedChunks_.begin(),
                    stagedChunks_.end(),
                    ReferenceAllocator{state_->session->allocator}};
                if (!buffer_.empty()) {
                    finalChunk = detail::stageBlobChunk<Limits>(
                        *state_, id_, chunks.size(), buffer_);
                    chunks.push_back(*finalChunk);
                }
                auto version = std::allocate_shared<BlobVersion>(
                    VersionAllocator{state_->session->allocator},
                    id_,
                    position_,
                    state_->session->opened.format.generation + 1,
                    std::move(chunks),
                    state_->stagingNextBlock *
                        Limits::allocationQuantumBytes);
                const auto previous = state_->blobs.find(id_);
                const auto discarded = previous != state_->blobs.end() &&
                        previous->second->pending
                    ? previous->second->chunks.size()
                    : 0;
                state_->discardedStagedReferences.reserve(
                    state_->discardedStagedReferences.size() + discarded);
                auto priorVersion = previous == state_->blobs.end()
                    ? BlobVersionPtr{}
                    : previous->second;
                state_->blobs.insert_or_assign(id_, std::move(version));
                if (priorVersion && priorVersion->pending) {
                    state_->discardedStagedReferences.insert(
                        state_->discardedStagedReferences.end(),
                        priorVersion->chunks.begin(),
                        priorVersion->chunks.end());
                }
                state_->abortableStagingReferences -=
                    stagedChunks_.size() + (finalChunk ? 1U : 0U);
                state_->openWriterIds.erase(id_);
                --state_->openWriters;
                active_ = false;
            } catch (const DatabaseError& error) {
                if (finalChunk) {
                    detail::releaseBlobStagingReference<Limits>(
                        *state_, *finalChunk);
                }
                handleCorruption(error);
                throw;
            } catch (...) {
                if (finalChunk) {
                    detail::releaseBlobStagingReference<Limits>(
                        *state_, *finalChunk);
                }
                throw;
            }
            state_.reset();
            sessionLifetime_.reset();
        }

        void abort() noexcept {
            if (!active_) {
                return;
            }
            active_ = false;
            if (state_ && sessionLifetime_ &&
                !sessionLifetime_->invalidated.load(std::memory_order_acquire) &&
                state_->active.load(std::memory_order_acquire)) {
                try {
                    detail::releaseBlobStagingReferences<Limits>(
                        *state_, stagedChunks_);
                    state_->openWriterIds.erase(id_);
                    --state_->openWriters;
                    --state_->mutations;
                } catch (...) {
                }
            }
            state_.reset();
            sessionLifetime_.reset();
        }

        [[nodiscard]] bool active() const noexcept {
            return active_ && state_ && sessionLifetime_ &&
                !sessionLifetime_->invalidated.load(std::memory_order_acquire) &&
                state_->active.load(std::memory_order_acquire);
        }

    private:
        friend class Database;
        friend class WriteTransaction;

        BlobWriter(
            std::shared_ptr<WriteBlobState> state,
            std::shared_ptr<ChildLifetime> sessionLifetime,
            BlobId id,
            bool active = true)
            : state_(std::move(state)),
              sessionLifetime_(std::move(sessionLifetime)),
              buffer_(typename std::allocator_traits<Allocator>::
                  template rebind_alloc<std::byte>{state_->session->allocator}),
              stagedChunks_(
                  typename std::allocator_traits<Allocator>::
                      template rebind_alloc<detail::ExtentReference>{
                          state_->session->allocator}),
              id_(id),
              position_(0),
              active_(active) {}

        void requireFunctional() const {
            if (!active()) {
                throw ContractError{Errc::InvalidState, "Blob writer is inactive"};
            }
            if (state_->thread != std::this_thread::get_id()) {
                throw ContractError{
                    Errc::WrongThread,
                    "Blob writer belongs to another thread"};
            }
            if (state_->session->confirmedCorruptionPending.load(
                    std::memory_order_acquire)) {
                throw DatabaseError{
                    Errc::RecoveryRequired,
                    "database requires recovery"};
            }
        }

        void handleCorruption(const DatabaseError& error) noexcept {
            if (error.code() != Errc::Corrupt || !state_ ||
                !sessionLifetime_) {
                return;
            }
            state_->active.store(false, std::memory_order_release);
            state_->openWriters = 0;
            Database::enterRecoveryAfterCorruption(
                *state_->session, *sessionLifetime_);
        }

        std::shared_ptr<WriteBlobState> state_;
        std::shared_ptr<ChildLifetime> sessionLifetime_;
        StoredBytes buffer_;
        detail::ExtentReferences<Allocator> stagedChunks_;
        BlobId id_;
        std::uint64_t position_;
        bool active_;
    };

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
              blobs_(std::move(other.blobs_)),
              blobReaderLifetime_(std::move(other.blobReaderLifetime_)),
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
            assert(!active_ || !blobReaderLifetime_ ||
                blobReaderLifetime_->liveReaders.load(
                    std::memory_order_relaxed) == 0);
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

        [[nodiscard]] std::optional<BlobReader> openBlob(BlobId id) {
            requireFunctional();
            const auto found = blobs_.find(id);
            if (found == blobs_.end()) {
                return std::nullopt;
            }
            return makeBlobReader(
                *session_, lifetime_, blobReaderLifetime_, found->second, thread_);
        }

        void end() noexcept {
            if (!active_) {
                return;
            }
            active_ = false;
            detail::invalidateCursors(cursorLifetime_);
            if (blobReaderLifetime_) {
                blobReaderLifetime_->invalidated.store(
                    true, std::memory_order_release);
            }
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
            BlobCatalog blobs,
            std::shared_ptr<BlobReaderLifetime> blobReaderLifetime,
            std::uint64_t readerIdentity)
            : session_(&session),
              lifetime_(std::move(lifetime)),
              values_(std::move(values)),
              tree_(std::move(tree)),
              cursorLifetime_(std::move(cursorLifetime)),
              blobs_(std::move(blobs)),
              blobReaderLifetime_(std::move(blobReaderLifetime)),
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
            if (session_->confirmedCorruptionPending.load(
                    std::memory_order_acquire)) {
                throw DatabaseError{
                    Errc::RecoveryRequired,
                    "database requires recovery"};
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
        BlobCatalog blobs_;
        std::shared_ptr<BlobReaderLifetime> blobReaderLifetime_;
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
              blobState_(std::move(other.blobState_)),
              blobReaderLifetime_(std::move(other.blobReaderLifetime_)),
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
            assert(!active_ || !blobReaderLifetime_ ||
                blobReaderLifetime_->liveReaders.load(
                    std::memory_order_relaxed) == 0);
            assert(!active_ || !blobState_ || blobState_->openWriters == 0);
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

        [[nodiscard]] std::optional<BlobReader> openBlob(BlobId id) {
            requireFunctional();
            const auto found = blobState_->blobs.find(id);
            if (found == blobState_->blobs.end()) {
                return std::nullopt;
            }
            return makeBlobReader(
                *session_, lifetime_, blobReaderLifetime_, found->second, thread_);
        }

        [[nodiscard]] BlobWriter createBlob() {
            requireFunctional();
            std::shared_lock operation{session_->operationMutex};
            requireFunctional();
            requireBlobWriterCapacity();
            std::array<std::byte, BlobId::encodedSize> randomBytes{};
            std::optional<BlobId> selected;
            for (unsigned attempt = 0; attempt != 256; ++attempt) {
                detail::ProviderAccess::crypto(*session_->providers)
                    .randomBytes(randomBytes);
                const auto candidate = BlobId::fromBytes(randomBytes);
                if (!blobState_->blobs.contains(candidate) &&
                    !blobState_->generatedIds.contains(candidate)) {
                    selected = candidate;
                    break;
                }
            }
            if (!selected) {
                session_->capacityFailureCount.fetch_add(
                    1, std::memory_order_relaxed);
                throw DatabaseError{
                    Errc::ResourceLimit,
                    "could not reserve a fresh Blob identifier"};
            }
            BlobWriter writer{blobState_, lifetime_, *selected, false};
            blobState_->generatedIds.insert(*selected);
            try {
                blobState_->openWriterIds.insert(*selected);
            } catch (...) {
                blobState_->generatedIds.erase(*selected);
                throw;
            }
            ++blobState_->mutations;
            ++blobState_->openWriters;
            writer.active_ = true;
            return writer;
        }

        [[nodiscard]] std::optional<BlobWriter> replaceBlob(BlobId id) {
            requireFunctional();
            const auto found = blobState_->blobs.find(id);
            if (found == blobState_->blobs.end()) {
                return std::nullopt;
            }
            if (blobState_->openWriterIds.contains(id)) {
                throw ContractError{
                    Errc::InvalidState,
                    "Blob already has an unfinished writer"};
            }
            requireBlobWriterCapacity();
            BlobWriter writer{blobState_, lifetime_, id, false};
            blobState_->openWriterIds.insert(id);
            ++blobState_->mutations;
            ++blobState_->openWriters;
            writer.active_ = true;
            return writer;
        }

        [[nodiscard]] bool eraseBlob(BlobId id) {
            requireFunctional();
            if (blobState_->openWriterIds.contains(id)) {
                throw ContractError{
                    Errc::InvalidState,
                    "cannot erase a Blob with an unfinished writer"};
            }
            const auto found = blobState_->blobs.find(id);
            if (found == blobState_->blobs.end()) {
                return false;
            }
            requireBlobMutationCapacity();
            if (found->second->pending) {
                blobState_->discardedStagedReferences.insert(
                    blobState_->discardedStagedReferences.end(),
                    found->second->chunks.begin(),
                    found->second->chunks.end());
            }
            blobState_->blobs.erase(found);
            ++blobState_->mutations;
            return true;
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
            const auto keyGrowth = changed_ && !values_.empty()
                ? std::max<std::uint64_t>(
                      16U * 1024U,
                      Limits::allocationQuantumBytes)
                : 0;
            const auto blobGrowth = blobState_->mutations == 0
                ? 0
                : std::max<std::uint64_t>(
                      blobState_->bytesWritten,
                      std::max<std::uint64_t>(
                          16U * 1024U,
                          Limits::allocationQuantumBytes));
            const auto estimatedGrowth =
                keyGrowth > std::numeric_limits<std::uint64_t>::max() -
                        blobGrowth
                ? std::numeric_limits<std::uint64_t>::max()
                : keyGrowth + blobGrowth;
            return WriteTransactionStats{
                keyMutations_,
                blobState_->mutations,
                blobState_->bytesWritten,
                estimatedGrowth,
                blobState_->openWriters};
        }

        void commit() {
            requireFunctional();
            if (blobState_->openWriters != 0) {
                throw ContractError{
                    Errc::InvalidState,
                    "commit requires every Blob writer to be finished or aborted"};
            }
            if (!changed_ && blobState_->mutations == 0) {
                active_ = false;
                detail::invalidateCursors(cursorLifetime_);
                invalidateBlobHandles();
                releaseTransaction(*session_, *lifetime_, true);
                return;
            }
            try {
                {
                    std::shared_lock operation{session_->operationMutex};
                    requireFunctional();
                    detail::commitExact<Limits>(
                        *session_, values_, blobState_->blobs, *blobState_);
                }
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
                    invalidateBlobHandles();
                    releaseTransaction(*session_, *lifetime_, true);
                }
                throw;
            } catch (...) {
                if (session_->state.load(std::memory_order_acquire) ==
                    DatabaseState::RecoveryRequired) {
                    active_ = false;
                    detail::invalidateCursors(cursorLifetime_);
                    invalidateBlobHandles();
                    releaseTransaction(*session_, *lifetime_, true);
                }
                throw;
            }
            active_ = false;
            detail::invalidateCursors(cursorLifetime_);
            invalidateBlobHandles();
            releaseTransaction(*session_, *lifetime_, true);
        }

        void rollback() noexcept {
            if (!active_) {
                return;
            }
            active_ = false;
            detail::invalidateCursors(cursorLifetime_);
            invalidateBlobHandles();
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
            std::shared_ptr<CursorLifetime> cursorLifetime,
            bool active = true)
            : session_(&session),
              lifetime_(std::move(lifetime)),
              values_(std::move(values)),
              tree_(std::move(tree)),
              cursorLifetime_(std::move(cursorLifetime)),
              blobState_(makeWriteBlobState(session)),
              blobReaderLifetime_(makeBlobReaderLifetime(session.allocator)),
              thread_(std::this_thread::get_id()),
              keyMutations_(0),
              changed_(false),
              treeCurrent_(true),
              active_(active) {
            cursorLifetime_->root = &tree_;
        }

        void requireFunctional() const {
            if (!active()) {
                throw ContractError{Errc::InvalidState, "write transaction is inactive"};
            }
            if (thread_ != std::this_thread::get_id()) {
                throw ContractError{Errc::WrongThread, "write transaction belongs to another thread"};
            }
            if (session_->confirmedCorruptionPending.load(
                    std::memory_order_acquire)) {
                throw DatabaseError{
                    Errc::RecoveryRequired,
                    "database requires recovery"};
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

        void requireBlobWriterCapacity() const {
            requireBlobMutationCapacity();
            if (blobState_->openWriters ==
                    Limits::maxOpenBlobWritersPerTransaction) {
                session_->capacityFailureCount.fetch_add(
                    1, std::memory_order_relaxed);
                throw DatabaseError{
                    Errc::ResourceLimit,
                    "transaction Blob-writer limit reached"};
            }
        }

        void requireBlobMutationCapacity() const {
            if (blobState_->mutations ==
                Limits::maxBlobMutationsPerTransaction) {
                session_->capacityFailureCount.fetch_add(
                    1, std::memory_order_relaxed);
                throw DatabaseError{
                    Errc::ResourceLimit,
                    "transaction Blob-mutation limit reached"};
            }
        }

        void invalidateBlobHandles() noexcept {
            if (blobState_) {
                blobState_->active.store(false, std::memory_order_release);
            }
            if (blobReaderLifetime_) {
                blobReaderLifetime_->invalidated.store(
                    true, std::memory_order_release);
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
        std::shared_ptr<WriteBlobState> blobState_;
        std::shared_ptr<BlobReaderLifetime> blobReaderLifetime_;
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
                std::move(validated.blobs),
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
            std::move(validated.blobs),
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
        try {
            std::shared_lock operation{session.operationMutex};
            std::lock_guard lock{session.mutex};
            const auto current = session.state.load(std::memory_order_acquire);
            if (current != DatabaseState::Open &&
                current != DatabaseState::RecoveryRequired) {
                throw ContractError{
                    Errc::InvalidState,
                    "database diagnostics require an open session"};
            }
            if (current == DatabaseState::Open) {
                ensureAllocatorSnapshotLoaded(session);
            }
        } catch (const DatabaseError& error) {
            if (error.code() == Errc::Corrupt) {
                enterRecoveryAfterCorruption(session, *lifetime_);
            }
            throw;
        }
        std::lock_guard lock{session.mutex};
        const auto current = session.state.load(std::memory_order_acquire);
        if (current != DatabaseState::Open &&
            current != DatabaseState::RecoveryRequired) {
            throw ContractError{
                Errc::InvalidState,
                "database diagnostics require an open session"};
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
        result.cacheUsedBytes = session.blobCache->usedBytes.load(
            std::memory_order_relaxed);
        result.cachePinnedBytes = result.cacheUsedBytes;
        result.cacheEvictions = session.blobCache->evictions.load(
            std::memory_order_relaxed);
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
        try {
            std::shared_lock operation{session.operationMutex};
            std::lock_guard lock{session.mutex};
            requireOpen(session);
            ensureValuesLoaded(session);
            ensureBlobsLoaded(session);
        } catch (const DatabaseError& error) {
            if (error.code() == Errc::Corrupt) {
                enterRecoveryAfterCorruption(session, *lifetime_);
            }
            throw;
        }
        std::lock_guard lock{session.mutex};
        requireOpen(session);
        if (session.activeReaders.size() == session.maxReaders ||
            session.nextReaderIdentity ==
                std::numeric_limits<std::uint64_t>::max()) {
            session.capacityFailureCount.fetch_add(1, std::memory_order_relaxed);
            throw DatabaseError{Errc::ResourceLimit, "reader limit reached"};
        }
        auto snapshot = detail::makeOrderedKeyValues(session.allocator);
        snapshot = session.values;
        auto blobSnapshot = makeBlobCatalog(session.allocator);
        blobSnapshot = session.blobs;
        auto tree = snapshotCursorTree(session);
        auto cursorLifetime = detail::makeOrderedCursorLifetime(
            tree, lifetime_, session.allocator);
        auto blobReaderLifetime = makeBlobReaderLifetime(session.allocator);
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
            std::move(blobSnapshot),
            std::move(blobReaderLifetime),
            readerIdentity};
    }

    [[nodiscard]] WriteTransaction beginWrite() {
        auto& session = requireSession();
        try {
            std::shared_lock operation{session.operationMutex};
            std::lock_guard lock{session.mutex};
            requireOpen(session);
            ensureValuesLoaded(session);
            ensureBlobsLoaded(session);
            ensureAllocatorSnapshotLoaded(session);
        } catch (const DatabaseError& error) {
            if (error.code() == Errc::Corrupt) {
                enterRecoveryAfterCorruption(session, *lifetime_);
            }
            throw;
        }
        std::unique_lock lock{session.mutex};
        requireOpen(session);
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
        std::optional<WriteTransaction> transaction;
        try {
            auto snapshot = detail::makeOrderedKeyValues(session.allocator);
            MutableTree tree{session.allocator};
            snapshot = session.values;
            tree = snapshotCursorTree(session);
            auto cursorLifetime = detail::makeOrderedCursorLifetime(
                tree, lifetime_, session.allocator);
            transaction.emplace(WriteTransaction{
                session,
                lifetime_,
                std::move(snapshot),
                std::move(tree),
                std::move(cursorLifetime),
                false});
        } catch (...) {
            --session.waitingWriters;
            ++session.servingWriterTicket;
            session.writerAvailable.notify_all();
            throw;
        }
        --session.waitingWriters;
        session.writerActive = true;
        ++session.liveTransactions;
        transaction->active_ = true;
        return std::move(*transaction);
    }

    [[nodiscard]] Result<WriteTransaction, WriterBusy> tryBeginWrite() {
        auto& session = requireSession();
        try {
            std::shared_lock operation{session.operationMutex};
            std::lock_guard lock{session.mutex};
            requireOpen(session);
            ensureValuesLoaded(session);
            ensureBlobsLoaded(session);
            ensureAllocatorSnapshotLoaded(session);
        } catch (const DatabaseError& error) {
            if (error.code() == Errc::Corrupt) {
                enterRecoveryAfterCorruption(session, *lifetime_);
            }
            throw;
        }
        std::lock_guard lock{session.mutex};
        requireOpen(session);
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
        auto result = Result<WriteTransaction, WriterBusy>::success(
            WriteTransaction{
                session,
                lifetime_,
                std::move(snapshot),
                std::move(tree),
                std::move(cursorLifetime),
                false});
        ++session.nextWriterTicket;
        session.writerActive = true;
        ++session.liveTransactions;
        result.value().active_ = true;
        return result;
    }

    void close() {
        auto& session = requireSession();
        std::unique_lock operation{session.operationMutex};
        if (session.confirmedCorruptionPending.load(
                std::memory_order_acquire) &&
            session.state.load(std::memory_order_acquire) ==
                DatabaseState::Open) {
            operation.unlock();
            std::unique_lock lock{session.mutex};
            session.writerAvailable.wait(lock, [&] {
                return session.state.load(std::memory_order_acquire) !=
                    DatabaseState::Open;
            });
            lock.unlock();
            operation.lock();
        }
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
        const auto recoveryCause = session.recoveryCause.load(
            std::memory_order_acquire);
        if (expected == DatabaseState::Open ||
            (expected == DatabaseState::RecoveryRequired &&
             recoveryCause == RecoveryCause::ClosePersistenceFailed)) {
            consolidateAbandonedTailForClose(session, expected);
        }
        operation.unlock();
        shutdownSession(session, *lifetime_);
    }

private:
    struct ValidatedFile {
        std::unique_ptr<detail::DurableFile> file;
        detail::OpenedDatabase opened;
        OrderedKeyValues values;
        BlobCatalog blobs;
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
        BlobCatalog blobs,
        std::size_t cacheCapacityBytes,
        std::uint32_t maxReaders) {
        SessionAllocator sessionAllocator{allocator};
        auto session = std::allocate_shared<Session>(
            sessionAllocator,
            std::move(file),
            std::move(providers),
            std::move(allocator),
            std::move(opened),
            std::move(values),
            cacheCapacityBytes,
            maxReaders);
        session->blobs = std::move(blobs);
        session->blobsLoaded = true;
        return session;
    }

    Database(
        std::unique_ptr<detail::DurableFile> file,
        ProviderSet providers,
        Allocator allocator,
        detail::OpenedDatabase opened,
        OrderedKeyValues values,
        BlobCatalog blobs,
        std::size_t cacheCapacityBytes,
        std::uint32_t maxReaders)
        : lifetime_(makeChildLifetime(allocator)),
          session_(makeSession(
              std::move(file),
              std::move(providers),
              std::move(allocator),
              std::move(opened),
              std::move(values),
              std::move(blobs),
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
        if (session.confirmedCorruptionPending.load(
                std::memory_order_acquire)) {
            throw DatabaseError{
                Errc::RecoveryRequired,
                "database requires recovery"};
        }
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
        const auto current = session.state.load(std::memory_order_acquire);
        if (current != DatabaseState::Open &&
            current != DatabaseState::RecoveryRequired) {
            return;
        }
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
        session.confirmedCorruptionPending.store(
            true, std::memory_order_release);
        try {
            std::unique_lock operation{session.operationMutex};
            std::lock_guard lock{session.mutex};
            enterRecoveryAfterCorruptionLocked(session, lifetime);
        } catch (...) {
            auto expected = DatabaseState::Open;
            if (session.state.compare_exchange_strong(
                    expected,
                    DatabaseState::RecoveryRequired,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire) ||
                expected == DatabaseState::RecoveryRequired) {
                session.recoveryCause.store(
                    RecoveryCause::ConfirmedCorruption,
                    std::memory_order_release);
                lifetime.invalidated.store(true, std::memory_order_release);
                session.writerAvailable.notify_all();
            }
        }
    }

    static void ensureValuesLoaded(Session& session) {
        if (session.valuesLoaded) {
            return;
        }
        auto values = detail::loadExactValues<Limits>(
            *session.file,
            session.opened,
            *session.providers,
            session.allocator,
            nullptr,
            session.cursorTree.get());
        session.values = std::move(values);
        session.valuesLoaded = true;
    }

    static void ensureBlobsLoaded(Session& session) {
        if (session.blobsLoaded) {
            return;
        }
        session.blobs = detail::loadBlobCatalog<Limits>(
            *session.file,
            session.opened,
            *session.providers,
            session.allocator);
        session.blobsLoaded = true;
    }

    static void ensureAllocatorSnapshotLoaded(Session& session) {
        if (session.allocatorSnapshotLoaded) {
            return;
        }
        auto snapshot = detail::loadAllocatorSnapshot<Limits>(
            *session.file,
            session.opened,
            *session.providers,
            session.allocator);
        session.liveBlocks = snapshot.liveBlocks;
        session.retiredBlocksByGeneration = std::move(
            snapshot.retiredBlocksByGeneration);
        session.allocatorSnapshotLoaded = true;
    }

    static void consolidateAbandonedTailForClose(
        Session& session,
        DatabaseState previousState) {
        std::uint64_t physicalBytes = 0;
        try {
            physicalBytes = session.file->size();
        } catch (...) {
            session.state.store(previousState, std::memory_order_release);
            throw;
        }
        const auto highWaterBytes = session.opened.format.highWaterBytes;
        if (physicalBytes < highWaterBytes) {
            session.recoveryCause.store(
                RecoveryCause::ConfirmedCorruption,
                std::memory_order_release);
            session.state.store(
                DatabaseState::RecoveryRequired,
                std::memory_order_release);
            throw DatabaseError{
                Errc::Corrupt,
                "database file is shorter than its committed high-water mark"};
        }
        const bool retryingPersistence =
            previousState == DatabaseState::RecoveryRequired;
        if (physicalBytes == highWaterBytes && !retryingPersistence) {
            session.opened.abandonedTailBytes = 0;
            return;
        }
        try {
            if (physicalBytes > highWaterBytes) {
                session.file->resize(highWaterBytes);
            }
            session.file->stableStorageBarrier();
            session.opened.abandonedTailBytes = 0;
        } catch (...) {
            session.recoveryCause.store(
                RecoveryCause::ClosePersistenceFailed,
                std::memory_order_release);
            session.state.store(
                DatabaseState::RecoveryRequired,
                std::memory_order_release);
            throw;
        }
    }

    static void shutdownSession(
        Session& session,
        ChildLifetime& lifetime) noexcept {
        try {
            std::unique_lock operation{session.operationMutex};
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
        auto blobs = detail::loadBlobCatalog<Limits>(
            *file, openedDatabase, providers, allocator);
        (void)detail::shallowValidateAllocatorRoot<Limits>(
            *file, openedDatabase, providers, allocator);
        auto values = detail::makeOrderedKeyValues(allocator);
        return Result<ValidatedFile, AuthenticationFailed>::success(ValidatedFile{
            std::move(file),
            std::move(openedDatabase),
            std::move(values),
            std::move(blobs)});
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
