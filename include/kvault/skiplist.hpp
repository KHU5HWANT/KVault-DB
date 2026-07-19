#pragma once

// ============================================================================
// skiplist.hpp — A Probabilistic Skip List with Arena-Based Memory Ownership
// ============================================================================
//
// OWNERSHIP MODEL:
//   Every SkipListNode is allocated on the heap and its lifetime is managed
//   by a single std::unique_ptr stored in the SkipList::arena_ vector.
//
//   The forward[] pointers inside each node are NON-OWNING raw pointers
//   (Node*) used purely for O(log n) traversal across levels. Multiple
//   forward pointers at different levels may point to the same node —
//   this is safe because none of them own the node.
//
//   When the SkipList is destroyed, arena_ releases all nodes in one pass.
//   No cascading destructors, no double-free, no stack overflow.
//
// WHY NOT unique_ptr FOR FORWARD POINTERS?
//   In a Skip List, the same node is reachable from multiple levels:
//     - Node A's forward[0] points to Node B  (level-0 next)
//     - Node X's forward[2] also points to Node B  (express lane)
//   If both were unique_ptr<Node>, Node B would have two owners — violating
//   unique_ptr's exclusive-ownership invariant and causing double-free UB.
//
// ============================================================================

#include "kvault/types.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <random>
#include <vector>

namespace kvault {

// ---------------------------------------------------------------------------
// SkipListNode
// ---------------------------------------------------------------------------
// Each node holds a key-value pair and a vector of forward pointers —
// one per level the node participates in. All pointers are non-owning.
// ---------------------------------------------------------------------------
struct SkipListNode {
    Key   key;
    Value value;

    // forward[i] = next node at level i, or nullptr if this node is the
    // last node at that level. These are NON-OWNING traversal pointers.
    // Lifetime of the pointed-to nodes is managed by SkipList::arena_.
    std::vector<SkipListNode*> forward;

    // Data node constructor
    SkipListNode(Key k, Value v, int height)
        : key(std::move(k))
        , value(std::move(v))
        , forward(static_cast<size_t>(height), nullptr) {}

    // Sentinel (head) node constructor — no meaningful key or value
    explicit SkipListNode(int height)
        : forward(static_cast<size_t>(height), nullptr) {}
};

// ---------------------------------------------------------------------------
// SkipList
// ---------------------------------------------------------------------------
// A probabilistic ordered data structure with O(log n) insert, search,
// and delete. Designed as the backing structure for KVault's MemTable.
//
// Thread Safety: NOT thread-safe. The MemTable layer is responsible for
//                synchronization if concurrent access is needed.
// ---------------------------------------------------------------------------
class SkipList {
public:
    // -- Configuration Constants --------------------------------------------
    // Maximum number of levels. 16 levels can efficiently handle up to
    // 2^16 = 65,536 entries (with p=0.5). Sufficient for a MemTable that
    // flushes at ~4 MB.
    static constexpr int kMaxLevel = 16;

    // Probability of promoting a node to the next level.
    // p = 0.5 gives a balanced space-time tradeoff (expected 2 pointers/node).
    static constexpr double kProbability = 0.5;

    // -- Lifecycle ----------------------------------------------------------
    SkipList();

    // Destructor: arena_ is a vector<unique_ptr<Node>>. Its destructor
    // calls delete on every node. Because forward[] contains only raw
    // pointers, there is no cascading or recursive destruction — each
    // node's destructor is O(1). Total cleanup is O(n), iterative, and
    // stack-safe even for millions of nodes.
    ~SkipList() = default;

    // Non-copyable — the arena holds unique ownership of all nodes.
    SkipList(const SkipList&)            = delete;
    SkipList& operator=(const SkipList&) = delete;

    // Movable — transferring ownership of the arena is well-defined.
    SkipList(SkipList&&) noexcept            = default;
    SkipList& operator=(SkipList&&) noexcept = default;

    // -- Core Operations ----------------------------------------------------

    // Insert a key-value pair. If the key already exists, its value is
    // overwritten (upsert semantics). Complexity: O(log n) expected.
    void insert(const Key& key, const Value& value);

    // Search for a key. Returns the associated value, or std::nullopt
    // if the key is not present. Complexity: O(log n) expected.
    [[nodiscard]]
    std::optional<Value> search(const Key& key) const;

    // Remove a key from the structure. Returns true if the key was found
    // and removed, false otherwise.
    //
    // NOTE: The removed node remains allocated in arena_ (lazy cleanup).
    // It is unlinked from all forward chains and unreachable, but its
    // memory is reclaimed only when the entire SkipList is destroyed.
    // This is intentional — for MemTable usage, the entire SkipList is
    // discarded atomically after flushing to an SSTable.
    bool remove(const Key& key);

    // -- Observers ----------------------------------------------------------

    // Number of key-value entries currently stored.
    [[nodiscard]] size_t size() const noexcept { return size_; }

    // Whether the structure contains any entries.
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    // Current highest level in use (0 = empty, kMaxLevel = fully utilized).
    [[nodiscard]] int current_level() const noexcept { return current_level_; }

    // -- Forward Iterator ---------------------------------------------------
    // A read-only, forward-only iterator that walks the level-0 chain
    // in sorted key order. Used by MemTable::snapshot() to produce a
    // sorted dump for SSTable flushing.
    // -------------------------------------------------------------------
    class Iterator {
    public:
        Iterator() : node_(nullptr) {}
        explicit Iterator(SkipListNode* node) : node_(node) {}

        // Dereference: returns a pair of const references to key and value.
        std::pair<const Key&, const Value&> operator*() const {
            return { node_->key, node_->value };
        }

        // Pre-increment: advance to the next node on level 0.
        Iterator& operator++() {
            if (node_ != nullptr) {
                node_ = node_->forward[0];
            }
            return *this;
        }

        // Post-increment
        Iterator operator++(int) {
            Iterator snapshot = *this;
            ++(*this);
            return snapshot;
        }

        bool operator==(const Iterator& other) const noexcept {
            return node_ == other.node_;
        }

        bool operator!=(const Iterator& other) const noexcept {
            return node_ != other.node_;
        }

        // Named accessors (more explicit than structured bindings)
        [[nodiscard]] const Key&   key()   const { return node_->key; }
        [[nodiscard]] const Value& value() const { return node_->value; }

    private:
        SkipListNode* node_; // Non-owning; node lifetime managed by arena_
    };

    // begin() points to the first data node (head's level-0 successor).
    // end() is the null sentinel.
    [[nodiscard]] Iterator begin() const {
        return Iterator(head_->forward[0]);
    }

    [[nodiscard]] Iterator end() const {
        return Iterator(nullptr);
    }

private:
    // Generate a random level in [1, kMaxLevel] using a geometric
    // distribution with parameter kProbability.
    int random_level();

    // -----------------------------------------------------------------------
    // MEMORY LAYOUT
    // -----------------------------------------------------------------------
    //
    //  head_ (unique_ptr)          arena_ (vector<unique_ptr>)
    //  ┌───────────┐               ┌──────────────────────────────┐
    //  │ Sentinel   │               │ [0] unique_ptr → Node("a")  │
    //  │ forward[0]─┼──► Node "a"   │ [1] unique_ptr → Node("b")  │
    //  │ forward[1]─┼──► Node "c"   │ [2] unique_ptr → Node("c")  │
    //  │ forward[2]─┼──► ...        │ [3] unique_ptr → Node("d")  │
    //  └───────────┘               └──────────────────────────────┘
    //                                ▲
    //                                │ OWNS all nodes. forward[] ptrs
    //                                │ in nodes are non-owning raw Node*.
    //                                │
    //                                └─ Destroyed when SkipList dies.
    //
    // -----------------------------------------------------------------------

    // Sentinel head node. Contains no data; its forward[] array is the
    // entry point into the structure at every level.
    std::unique_ptr<SkipListNode> head_;

    // Centralized memory arena. Every data node created by insert() is
    // placed here. Guarantees single ownership and O(n) bulk cleanup.
    //
    // Why vector<unique_ptr> is safe here:
    //   When the vector reallocates (grows), the unique_ptr objects are
    //   moved to the new buffer. But the underlying Node* heap addresses
    //   do NOT change — unique_ptr::get() returns the same pointer before
    //   and after the move. So all raw Node* in forward[] remain valid.
    std::vector<std::unique_ptr<SkipListNode>> arena_;

    int    current_level_; // Highest level with at least one node (0 = empty)
    size_t size_;          // Number of entries (not including sentinel)

    // Random number generation for level assignment.
    // mutable because search() is const but may theoretically need the
    // RNG in debug/instrumentation scenarios. Here it's used only in
    // insert(), but keeping it mutable avoids future refactoring.
    mutable std::mt19937                       rng_;
    mutable std::uniform_real_distribution<double> dist_;
};

} // namespace kvault
