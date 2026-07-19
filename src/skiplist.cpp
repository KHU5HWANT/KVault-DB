#include "kvault/skiplist.hpp"

#include <cassert>
#include <chrono>

// Concise int → size_t cast for vector subscripting.
// Eliminates -Wsign-conversion warnings without verbose static_cast<size_t>()
// at every call site.
namespace {
inline constexpr size_t sz(int i) noexcept {
    return static_cast<size_t>(i);
}
} // anonymous namespace

namespace kvault {

// ============================================================================
// Construction
// ============================================================================

SkipList::SkipList()
    : head_(std::make_unique<SkipListNode>(kMaxLevel))
    , current_level_(0)
    , size_(0)
    , rng_(static_cast<unsigned>(
          std::chrono::steady_clock::now().time_since_epoch().count()))
    , dist_(0.0, 1.0)
{
    // Reserve arena space to reduce early reallocations.
    // A 4 MB MemTable with ~100-byte entries ≈ 40K nodes.
    arena_.reserve(1024);
}

// ============================================================================
// Random Level Generation
// ============================================================================
// Generates a level in [1, kMaxLevel] with a geometric distribution.
// Each successive level has a kProbability chance of being included.
//
// Expected height of a node = 1 / (1 - p) = 2.0 for p = 0.5
// Expected total pointers across all nodes = n * 2.0
// ============================================================================

int SkipList::random_level() {
    int level = 1;
    while (level < kMaxLevel && dist_(rng_) < kProbability) {
        ++level;
    }
    return level;
}

// ============================================================================
// Insert (Upsert)
// ============================================================================
// 1. Descend from the highest active level, recording the last node at
//    each level whose key < target key (the "update" vector).
// 2. If the key exists at level 0, overwrite its value (upsert).
// 3. Otherwise, allocate a new node in the arena, generate a random
//    height, and splice it into all levels [0, height).
//
// Complexity: O(log n) expected time, O(1) amortized allocation.
// ============================================================================

void SkipList::insert(const Key& key, const Value& value) {
    // update[i] will hold the rightmost node at level i whose key < `key`.
    // After traversal, the new node is spliced between update[i] and
    // update[i]->forward[i] at each level.
    std::vector<SkipListNode*> update(kMaxLevel, nullptr);
    SkipListNode* current = head_.get();

    // Traverse: start at the highest active level, move right while
    // the next node's key is still less than the target, then drop down.
    for (int i = current_level_ - 1; i >= 0; --i) {
        while (current->forward[sz(i)] != nullptr &&
               current->forward[sz(i)]->key < key) {
            current = current->forward[sz(i)];
        }
        update[sz(i)] = current;
    }

    // After the traversal, current->forward[0] is the first node whose
    // key >= target key. Check for exact match (upsert case).
    SkipListNode* candidate = current->forward[0];
    if (candidate != nullptr && candidate->key == key) {
        // Key exists — update the value in place. No structural change.
        candidate->value = value;
        return;
    }

    // -- Allocate & splice a new node --

    const int new_height = random_level();

    // If the new node is taller than the current structure, extend the
    // update vector to include the head at those new levels.
    if (new_height > current_level_) {
        for (int i = current_level_; i < new_height; ++i) {
            update[sz(i)] = head_.get();
        }
        current_level_ = new_height;
    }

    // Create the new node in the arena. The arena takes unique ownership;
    // we extract a raw pointer for forward[] wiring.
    arena_.push_back(std::make_unique<SkipListNode>(key, value, new_height));
    SkipListNode* new_node = arena_.back().get();

    // Splice: at each level [0, new_height), insert new_node between
    // update[i] and update[i]->forward[i].
    //
    //  BEFORE:  update[i] ─────────────────► update[i]->forward[i]
    //  AFTER:   update[i] ──► new_node ──► update[i]->forward[i]
    //
    for (int i = 0; i < new_height; ++i) {
        new_node->forward[sz(i)]  = update[sz(i)]->forward[sz(i)];
        update[sz(i)]->forward[sz(i)] = new_node;
    }

    ++size_;
}

// ============================================================================
// Search
// ============================================================================
// Descend from the highest level, moving right along express lanes until
// we overshoot, then drop down. At level 0, check for an exact match.
//
// Complexity: O(log n) expected — each level skips ~half the remaining nodes.
// ============================================================================

std::optional<Value> SkipList::search(const Key& key) const {
    const SkipListNode* current = head_.get();

    for (int i = current_level_ - 1; i >= 0; --i) {
        while (current->forward[sz(i)] != nullptr &&
               current->forward[sz(i)]->key < key) {
            current = current->forward[sz(i)];
        }
    }

    // current->forward[0] is the first node with key >= target.
    const SkipListNode* candidate = current->forward[0];
    if (candidate != nullptr && candidate->key == key) {
        return candidate->value;
    }

    return std::nullopt;
}

// ============================================================================
// Remove
// ============================================================================
// 1. Find the node using the same traversal as insert, recording update[].
// 2. Unlink the node from all levels by rewiring forward pointers.
// 3. Shrink current_level_ if the topmost levels are now empty.
//
// IMPORTANT: The node is NOT removed from arena_. It remains allocated
// but unreachable (a "zombie"). This is a deliberate design choice:
//
//   - For MemTable usage, physical deletes are tombstones (handled at
//     the MemTable layer), and the entire SkipList is discarded after
//     flushing. Scanning arena_ to find-and-erase would be O(n) — worse
//     than the O(log n) unlink operation itself.
//
//   - If arena compaction were needed (e.g., long-lived skip lists),
//     a generation-based or epoch-based reclamation scheme could be
//     layered on top without changing this interface.
//
// Complexity: O(log n) expected for unlinking. O(1) for "deallocation"
//             (deferred to SkipList destruction).
// ============================================================================

bool SkipList::remove(const Key& key) {
    std::vector<SkipListNode*> update(kMaxLevel, nullptr);
    SkipListNode* current = head_.get();

    for (int i = current_level_ - 1; i >= 0; --i) {
        while (current->forward[sz(i)] != nullptr &&
               current->forward[sz(i)]->key < key) {
            current = current->forward[sz(i)];
        }
        update[sz(i)] = current;
    }

    SkipListNode* target = current->forward[0];

    // Key not found — nothing to remove.
    if (target == nullptr || target->key != key) {
        return false;
    }

    // Unlink target from every level it participates in.
    //
    //  BEFORE:  update[i] ──► target ──► target->forward[i]
    //  AFTER:   update[i] ──────────────► target->forward[i]
    //
    // We stop as soon as update[i]->forward[i] != target, because the
    // target cannot appear at any higher level without appearing at all
    // lower levels (skip list invariant).
    for (int i = 0; i < current_level_; ++i) {
        if (update[sz(i)]->forward[sz(i)] != target) {
            break;
        }
        update[sz(i)]->forward[sz(i)] = target->forward[sz(i)];
    }

    // Shrink the structure height if the highest levels are now empty.
    // This keeps traversal efficient by avoiding wasted empty levels.
    while (current_level_ > 0 &&
           head_->forward[static_cast<size_t>(current_level_ - 1)] == nullptr) {
        --current_level_;
    }

    --size_;
    return true;

    // target is now unreachable via forward pointers but still lives in
    // arena_. Its memory will be freed when this SkipList is destroyed.
}

} // namespace kvault
