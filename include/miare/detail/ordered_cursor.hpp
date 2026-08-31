#pragma once

#include <miare/detail/exact_store.hpp>

#include <atomic>
#include <cassert>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

namespace miare::detail {

struct SessionChildLifetime {
    std::atomic<bool> invalidated{false};
};

enum class OrderedRangeKind {
    All,
    HalfOpen,
    Prefix
};

template<class Allocator>
struct OrderedCursorLifetime {
    MutableTreeNode<Allocator>* root;
    std::shared_ptr<SessionChildLifetime> sessionLifetime;
    std::thread::id thread;
    std::uint64_t mutationEpoch = 0;
    std::size_t liveCursors = 0;
    bool active = true;
};

template<class Allocator>
[[nodiscard]] inline std::shared_ptr<OrderedCursorLifetime<Allocator>>
makeOrderedCursorLifetime(
    MutableTreeNode<Allocator>& tree,
    std::shared_ptr<SessionChildLifetime> sessionLifetime,
    const Allocator& allocator) {
    using Lifetime = OrderedCursorLifetime<Allocator>;
    using LifetimeAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<Lifetime>;
    return std::allocate_shared<Lifetime>(
        LifetimeAllocator{allocator},
        Lifetime{
            &tree,
            std::move(sessionLifetime),
            std::this_thread::get_id()});
}

/// Navigation handle exposed as `Database::ReadCursor` or `Database::WriteCursor`.
///
/// A cursor is move-constructible, non-copyable, and initially unpositioned.
/// It is bound to its transaction thread. Returned key and value views remain
/// valid until movement, destruction, transaction termination, or write-cursor
/// invalidation.
template<class Allocator, class Limits, bool Write>
class OrderedCursor {
private:
    using CursorLifetime = OrderedCursorLifetime<Allocator>;
    using StoredBytes = detail::StoredBytes<Allocator>;
    using TreeNode = MutableTreeNode<Allocator>;
    using Path = StoredVector<std::size_t, Allocator>;

public:
    OrderedCursor(const OrderedCursor&) = delete;
    OrderedCursor& operator=(const OrderedCursor&) = delete;

    OrderedCursor(OrderedCursor&& other) noexcept
        : lifetime_(std::move(other.lifetime_)),
          lower_(std::move(other.lower_)),
          upper_(std::move(other.upper_)),
          path_(std::move(other.path_)),
          valueIndex_(other.valueIndex_),
          epoch_(other.epoch_),
          positioned_(other.positioned_),
          active_(std::exchange(other.active_, false)) {}

    OrderedCursor& operator=(OrderedCursor&&) = delete;

    ~OrderedCursor() {
        releaseResource();
    }

    OrderedCursor(
        std::shared_ptr<CursorLifetime> lifetime,
        std::optional<StoredBytes> lower,
        std::optional<StoredBytes> upper,
        const Allocator& allocator)
        : lifetime_(std::move(lifetime)),
          lower_(std::move(lower)),
          upper_(std::move(upper)),
          path_(typename std::allocator_traits<Allocator>::
              template rebind_alloc<std::size_t>{allocator}),
          valueIndex_(0),
          epoch_(lifetime_->mutationEpoch),
          positioned_(false),
          active_(true) {
        path_.reserve(maximumTreeLevel);
        ++lifetime_->liveCursors;
    }

    /// Positions at the first key in the configured range.
    /// @return `true` if a key was found.
    [[nodiscard]] bool first() {
        requireFunctional();
        const auto found = lower_
            ? descendLowerBound(*lower_)
            : descendLeft();
        return found && acceptUpperBound();
    }

    /// Positions at the last key in the configured range.
    /// @return `true` if a key was found.
    [[nodiscard]] bool last() {
        requireFunctional();
        bool found;
        if (upper_) {
            found = descendLowerBound(*upper_);
            if (found) {
                found = moveToPreviousValue();
            } else {
                found = descendRight();
            }
        } else {
            found = descendRight();
        }
        if (!found || !acceptLowerBound()) {
            positioned_ = false;
            return false;
        }
        return true;
    }

    /// Positions at the first in-range key not less than `key`.
    /// @return `true` if a key was found.
    [[nodiscard]] bool seekLowerBound(ByteView key) {
        requireFunctional();
        if (key.size() > Limits::maxKeyBytes) {
            throw ContractError{
                Errc::InvalidArgument,
                "key exceeds the capacity profile"};
        }
        auto sought = key;
        if (lower_ && UnsignedBytesLess{}(sought, ByteView{*lower_})) {
            sought = *lower_;
        }
        const auto found = descendLowerBound(sought);
        return found && acceptUpperBound();
    }

    /// Moves to the next key in the range.
    /// @return `false` after crossing the range and becoming unpositioned.
    [[nodiscard]] bool next() {
        requirePositioned();
        if (!moveToNextValue() || !acceptUpperBound()) {
            positioned_ = false;
            return false;
        }
        return true;
    }

    /// Moves to the previous key in the range.
    /// @return `false` after crossing the range and becoming unpositioned.
    [[nodiscard]] bool previous() {
        requirePositioned();
        if (!moveToPreviousValue() || !acceptLowerBound()) {
            positioned_ = false;
            return false;
        }
        return true;
    }

    /// Returns whether the cursor is functional and positioned on a key.
    [[nodiscard]] bool positioned() const noexcept {
        return functional() && positioned_;
    }

    /// Returns a view of the current key.
    [[nodiscard]] ByteView key() const {
        requirePositioned();
        return leaf().values[valueIndex_].key;
    }

    /// Returns a view of the current value.
    [[nodiscard]] ByteView value() const {
        requirePositioned();
        return leaf().values[valueIndex_].value;
    }

private:
    [[nodiscard]] bool functional() const noexcept {
        return active_ && lifetime_ && lifetime_->active && lifetime_->root &&
            epoch_ == lifetime_->mutationEpoch &&
            !lifetime_->sessionLifetime->invalidated.load(
                std::memory_order_acquire);
    }

    [[nodiscard]] bool countsAsResource() const noexcept {
        return active_ && lifetime_ && lifetime_->active &&
            epoch_ == lifetime_->mutationEpoch;
    }

    void releaseResource() noexcept {
        if (countsAsResource()) {
            assert(lifetime_->liveCursors != 0);
            --lifetime_->liveCursors;
        }
    }

    void requireFunctional() const {
        if (!functional()) {
            throw ContractError{Errc::InvalidState, "cursor is inactive"};
        }
        if (lifetime_->thread != std::this_thread::get_id()) {
            throw ContractError{Errc::WrongThread, "cursor belongs to another thread"};
        }
    }

    void requirePositioned() const {
        requireFunctional();
        if (!positioned_) {
            throw ContractError{Errc::InvalidState, "cursor is unpositioned"};
        }
    }

    [[nodiscard]] const TreeNode& nodeAtDepth(std::size_t depth) const {
        const auto* node = lifetime_->root;
        for (std::size_t index = 0; index != depth; ++index) {
            node = &node->children[path_[index]];
        }
        return *node;
    }

    [[nodiscard]] const TreeNode& leaf() const {
        return nodeAtDepth(path_.size());
    }

    [[nodiscard]] bool descendLeft() {
        path_.clear();
        const auto* node = lifetime_->root;
        while (node->level != 0) {
            if (node->children.empty()) {
                positioned_ = false;
                return false;
            }
            path_.push_back(0);
            node = &node->children.front();
        }
        if (node->values.empty()) {
            positioned_ = false;
            return false;
        }
        valueIndex_ = 0;
        positioned_ = true;
        return true;
    }

    [[nodiscard]] bool descendRight() {
        path_.clear();
        const auto* node = lifetime_->root;
        while (node->level != 0) {
            if (node->children.empty()) {
                positioned_ = false;
                return false;
            }
            path_.push_back(node->children.size() - 1);
            node = &node->children.back();
        }
        if (node->values.empty()) {
            positioned_ = false;
            return false;
        }
        valueIndex_ = node->values.size() - 1;
        positioned_ = true;
        return true;
    }

    [[nodiscard]] bool descendLowerBound(ByteView sought) {
        path_.clear();
        const auto* node = lifetime_->root;
        while (node->level != 0) {
            if (node->children.empty()) {
                positioned_ = false;
                return false;
            }
            const auto child = mutableChildFor(*node, sought);
            path_.push_back(child);
            node = &node->children[child];
        }
        const auto found = std::lower_bound(
            node->values.begin(),
            node->values.end(),
            sought,
            [](const auto& entry, ByteView key) {
                return UnsignedBytesLess{}(entry.key, key);
            });
        valueIndex_ = static_cast<std::size_t>(found - node->values.begin());
        positioned_ = valueIndex_ != node->values.size();
        if (!positioned_) {
            return moveToNextLeaf();
        }
        return true;
    }

    [[nodiscard]] bool moveToNextLeaf() {
        for (auto depth = path_.size(); depth != 0; --depth) {
            const auto& parent = nodeAtDepth(depth - 1);
            auto& child = path_[depth - 1];
            if (child + 1 == parent.children.size()) {
                continue;
            }
            ++child;
            path_.resize(depth);
            const auto* node = &parent.children[child];
            while (node->level != 0) {
                path_.push_back(0);
                node = &node->children.front();
            }
            if (!node->values.empty()) {
                valueIndex_ = 0;
                positioned_ = true;
                return true;
            }
        }
        positioned_ = false;
        return false;
    }

    [[nodiscard]] bool moveToPreviousLeaf() {
        for (auto depth = path_.size(); depth != 0; --depth) {
            auto& child = path_[depth - 1];
            if (child == 0) {
                continue;
            }
            const auto& parent = nodeAtDepth(depth - 1);
            --child;
            path_.resize(depth);
            const auto* node = &parent.children[child];
            while (node->level != 0) {
                path_.push_back(node->children.size() - 1);
                node = &node->children.back();
            }
            if (!node->values.empty()) {
                valueIndex_ = node->values.size() - 1;
                positioned_ = true;
                return true;
            }
        }
        positioned_ = false;
        return false;
    }

    [[nodiscard]] bool moveToNextValue() {
        const auto& current = leaf();
        if (valueIndex_ + 1 != current.values.size()) {
            ++valueIndex_;
            return true;
        }
        return moveToNextLeaf();
    }

    [[nodiscard]] bool moveToPreviousValue() {
        if (positioned_ && valueIndex_ != 0) {
            --valueIndex_;
            return true;
        }
        return moveToPreviousLeaf();
    }

    [[nodiscard]] bool acceptLowerBound() const {
        return !lower_ || !UnsignedBytesLess{}(
            ByteView{leaf().values[valueIndex_].key}, ByteView{*lower_});
    }

    [[nodiscard]] bool acceptUpperBound() {
        if (upper_ && !UnsignedBytesLess{}(
                ByteView{leaf().values[valueIndex_].key}, ByteView{*upper_})) {
            positioned_ = false;
            return false;
        }
        return true;
    }

    std::shared_ptr<CursorLifetime> lifetime_;
    std::optional<StoredBytes> lower_;
    std::optional<StoredBytes> upper_;
    Path path_;
    std::size_t valueIndex_;
    std::uint64_t epoch_;
    bool positioned_;
    bool active_;
};

template<class Allocator, class Limits, bool Write>
[[nodiscard]] inline OrderedCursor<Allocator, Limits, Write> makeOrderedCursor(
    const std::shared_ptr<OrderedCursorLifetime<Allocator>>& lifetime,
    OrderedRangeKind kind,
    std::optional<ByteView> borrowedLower,
    std::optional<ByteView> borrowedUpper,
    const Allocator& allocator) {
    if (lifetime->liveCursors == Limits::maxCursorsPerTransaction) {
        throw DatabaseError{
            Errc::ResourceLimit,
            "transaction cursor limit reached"};
    }
    using ByteAllocator = typename std::allocator_traits<Allocator>::
        template rebind_alloc<std::byte>;
    std::optional<StoredBytes<Allocator>> lower;
    std::optional<StoredBytes<Allocator>> upper;
    if (kind == OrderedRangeKind::HalfOpen) {
        const auto maximumBoundBytes = Limits::maxKeyBytes + 1;
        if ((borrowedLower && borrowedLower->size() > maximumBoundBytes) ||
            (borrowedUpper && borrowedUpper->size() > maximumBoundBytes)) {
            throw ContractError{
                Errc::InvalidArgument,
                "range bound exceeds the capacity profile"};
        }
        if (borrowedLower) {
            lower.emplace(ByteAllocator{allocator});
            lower->assign(borrowedLower->begin(), borrowedLower->end());
        }
        if (borrowedUpper) {
            upper.emplace(ByteAllocator{allocator});
            upper->assign(borrowedUpper->begin(), borrowedUpper->end());
        }
    } else if (kind == OrderedRangeKind::Prefix) {
        if (borrowedLower->size() > Limits::maxKeyBytes) {
            throw ContractError{
                Errc::InvalidArgument,
                "key exceeds the capacity profile"};
        }
        lower.emplace(ByteAllocator{allocator});
        lower->assign(borrowedLower->begin(), borrowedLower->end());
        upper = lower;
        while (!upper->empty() && upper->back() == std::byte{0xff}) {
            upper->pop_back();
        }
        if (!upper->empty()) {
            upper->back() = std::byte{
                static_cast<unsigned char>(
                    std::to_integer<unsigned char>(upper->back()) + 1U)};
        } else {
            upper.reset();
        }
    }
    return OrderedCursor<Allocator, Limits, Write>{
        lifetime,
        std::move(lower),
        std::move(upper),
        allocator};
}

template<class Allocator>
inline void invalidateWriteCursors(
    const std::shared_ptr<OrderedCursorLifetime<Allocator>>& lifetime) noexcept {
    ++lifetime->mutationEpoch;
    lifetime->liveCursors = 0;
}

template<class Allocator>
inline void invalidateCursors(
    const std::shared_ptr<OrderedCursorLifetime<Allocator>>& lifetime) noexcept {
    if (lifetime) {
        lifetime->active = false;
        lifetime->liveCursors = 0;
        lifetime->root = nullptr;
    }
}

} // namespace miare::detail
