#pragma once

#include <cstdint>
#include <cstddef>
#include <cassert>
#include <utility>
#include <functional>
#include <type_traits>
#include "config.h"
#include "b_plus_tree_page.h"
#include "generic_key.h"

class BufferPoolManager;

/**
 * @class BPlusTreeInternalPage
 * @brief Store n indexed keys and n+1 child pointers within a fixed 4096-byte Page buffer.
 *
 * Internal page physical layout inside a Page buffer:
 * -------------------------------------------------------------------------------------------------
 * | CommonPageHeader (24 Bytes)                                                                   |
 * | page_type (4B) | lsn (4B) | size (4B) | max_size (4B) | parent_page_id (4B) | page_id (4B)   |
 * -------------------------------------------------------------------------------------------------
 * | array_[0] : (invalid/dummy key, child_page_id 0)                                              |
 * | array_[1] : (key 1,             child_page_id 1)                                              |
 * | array_[2] : (key 2,             child_page_id 2)                                              |
 * | ...                                                                                           |
 * | array_[size - 1] : (key size-1, child_page_id size-1)                                         |
 * -------------------------------------------------------------------------------------------------
 *
 * Invariants:
 * 1. KeyAt(0) is invalid/dummy. ValueAt(0) is the leftmost child pointer.
 * 2. Keys are kept sorted in strictly ascending order: KeyAt(1) < KeyAt(2) < ... < KeyAt(size-1).
 * 3. In the tree topology:
 *    - All keys in subtree ValueAt(0) are < KeyAt(1).
 *    - All keys in subtree ValueAt(i) are >= KeyAt(i) and < KeyAt(i+1) (for 1 <= i < size-1).
 *    - All keys in subtree ValueAt(size-1) are >= KeyAt(size-1).
 */
template <typename KeyType, typename ValueType = page_id_t, typename KeyComparator = std::less<KeyType>>
class BPlusTreeInternalPage : public BPlusTreePage {
public:
    using MappingType = std::pair<KeyType, ValueType>;

    static constexpr size_t INTERNAL_PAGE_HEADER_SIZE = sizeof(BPlusTreePage); // 24 bytes
    static constexpr size_t INTERNAL_PAGE_SIZE = BPlusTreePageSizeEstimator::ComputeInternalMaxSize<KeyType, ValueType>(PAGE_SIZE);

    BPlusTreeInternalPage() = default;

    /**
     * @brief Initialize an internal page frame.
     * @param page_id The physical page ID assigned to this page.
     * @param parent_page_id Optional parent page ID (default: INVALID_PAGE_ID).
     * @param max_size Maximum capacity of child pointers (default: INTERNAL_PAGE_SIZE).
     */
    void Init(page_id_t page_id, page_id_t parent_page_id = INVALID_PAGE_ID, int max_size = static_cast<int>(INTERNAL_PAGE_SIZE)) {
        SetPageType(PageType::BPLUS_TREE_INTERNAL_PAGE);
        SetSize(0);
        SetMaxSize((max_size > 0 && max_size <= static_cast<int>(INTERNAL_PAGE_SIZE)) ? max_size : static_cast<int>(INTERNAL_PAGE_SIZE));
        SetParentPageId(parent_page_id);
        SetPageId(page_id);
        SetLSN(0);
    }

    /**
     * @brief Minimum number of children required in a non-root internal page.
     * Equal to (max_size + 1) / 2 (consistent with ValidateTree occupancy check).
     */
    int GetMinSize() const {
        return (GetMaxSize() + 1) / 2;
    }

    /**
     * @brief Reinterpret cast a raw Page pointer into a typed BPlusTreeInternalPage pointer.
     * @param page Raw memory Page pointer.
     * @return BPlusTreeInternalPage* In-place typed pointer.
     */
    static BPlusTreeInternalPage *Cast(Page *page) {
        if (page == nullptr) {
            return nullptr;
        }
        return reinterpret_cast<BPlusTreeInternalPage *>(page->GetData());
    }

    /**
     * @brief Reinterpret cast a const raw Page pointer into a const BPlusTreeInternalPage pointer.
     * @param page Const raw memory Page pointer.
     * @return const BPlusTreeInternalPage* In-place typed const pointer.
     */
    static const BPlusTreeInternalPage *Cast(const Page *page) {
        if (page == nullptr) {
            return nullptr;
        }
        return reinterpret_cast<const BPlusTreeInternalPage *>(page->GetData());
    }

    // ==================== Element Accessors ====================

    /**
     * @brief Get key at a specific index.
     * @note KeyAt(0) is unused / invalid in B+ tree internal nodes.
     * @param index Entry index (1 <= index < GetSize()).
     * @return const KeyType& Key reference.
     */
    const KeyType &KeyAt(int index) const {
        assert(index >= 0 && index < GetSize());
        return array_[index].first;
    }

    /**
     * @brief Set key at a specific index.
     * @param index Entry index.
     * @param key Key value.
     */
    void SetKeyAt(int index, const KeyType &key) {
        assert(index >= 0 && index < GetMaxSize());
        array_[index].first = key;
    }

    /**
     * @brief Get child page ID at a specific index.
     * @param index Entry index (0 <= index < GetSize()).
     * @return ValueType Child page ID.
     */
    ValueType ValueAt(int index) const {
        assert(index >= 0 && index < GetSize());
        return array_[index].second;
    }

    /**
     * @brief Set child page ID at a specific index.
     * @param index Entry index.
     * @param value Child page ID.
     */
    void SetValueAt(int index, const ValueType &value) {
        assert(index >= 0 && index < GetMaxSize());
        array_[index].second = value;
    }

    /**
     * @brief Find the array index for a given child page ID.
     * @param value Child page ID to search for.
     * @return int Index if found, or -1 if not found.
     */
    int ValueIndex(const ValueType &value) const;

    // ==================== Binary-Search Child Lookup ====================

    /**
     * @brief Find the child page ID that can contain the given key via binary search.
     *
     * In an internal node with size n:
     * - ValueAt(0) covers keys < KeyAt(1)
     * - ValueAt(i) covers keys >= KeyAt(i) and < KeyAt(i+1)
     * - ValueAt(n-1) covers keys >= KeyAt(n-1)
     *
     * Binary search finds the upper-bound index in array_[1 .. size-1] such that
     * KeyAt(i) <= key. If key < KeyAt(1), returns ValueAt(0).
     *
     * @param key Search key.
     * @param comparator Optional custom comparator (defaults to KeyComparator{}).
     * @return ValueType Child page ID to descend into.
     */
    ValueType Lookup(const KeyType &key, const KeyComparator &comparator = KeyComparator{}) const;

    // ==================== Insert-After Helper ====================

    /**
     * @brief Insert a new (key, child_page_id) pair immediately following old_value.
     *
     * Used when a child node splits into old_value and new_value:
     * Finds index of old_value, shifts subsequent entries right by 1, and inserts
     * (new_key, new_value) at index + 1.
     *
     * @param old_value The existing child page ID.
     * @param new_key The separator key for the new child node.
     * @param new_value The new child page ID to insert.
     * @return int The updated size of the node, or -1 if old_value was not found.
     */
    int InsertNodeAfter(const ValueType &old_value, const KeyType &new_key, const ValueType &new_value);

    // ==================== Node Management Helpers ====================

    /**
     * @brief Populate a newly created root node with two children and one separator key.
     * @param value0 Pointer to leftmost child.
     * @param key1 Separator key.
     * @param value1 Pointer to right child.
     */
    void Populate(const ValueType &value0, const KeyType &key1, const ValueType &value1);

    /**
     * @brief Remove an entry at a given index and shift remaining entries left.
     * @param index Index to remove.
     */
    void Remove(int index);

    /**
     * @brief Return a const pointer to the internal entry array.
     */
    const MappingType *GetArray() const {
        return array_;
    }

    // ==================== Split & Merge Redistribution Helpers ====================

    /**
     * @brief Move upper half of entries from this node into recipient during internal node split.
     *
     * In an internal node with size n:
     * - Entries [0 .. move_start - 1] stay in this node (size = move_start).
     * - Entries [move_start .. n - 1] move to recipient.
     * - recipient->array_[0].second is set to array_[move_start].second (leftmost child of recipient).
     * - recipient->array_[0].first is set to array_[move_start].first (stored as split separator key).
     * - recipient->array_[1 .. move_count - 1] get array_[move_start + 1 .. n - 1].
     * - If buffer pool manager is provided, reparents all moved children to recipient->GetPageId().
     *
     * @param recipient Target sibling internal page.
     * @param bpm Optional BufferPoolManager to update parent pointers of all moved child pages.
     */
    void MoveHalfTo(BPlusTreeInternalPage *recipient, BufferPoolManager *bpm = nullptr);

    /**
     * @brief Move all entries from this node into recipient during internal node merge.
     *
     * In an internal node merge:
     * - recipient has size S_L.
     * - array_[0].second (this node's leftmost child) is inserted at recipient->array_[S_L] with middle_key.
     * - array_[1 .. size - 1] are copied to recipient->array_[S_L + 1 .. S_L + size - 1].
     * - recipient size increases by size.
     * - If bpm is provided, all moved children are reparented to recipient->GetPageId().
     * - This node size is reset to 0.
     *
     * @param recipient Target left sibling internal page.
     * @param middle_key Separator key pulled down from parent.
     * @param bpm Optional BufferPoolManager to update parent pointers of all moved child pages.
     */
    void MoveAllTo(BPlusTreeInternalPage *recipient, const KeyType &middle_key, BufferPoolManager *bpm = nullptr);

    /**
     * @brief Move last entry from this node to front of recipient (redistribution from left sibling).
     *
     * In an internal node:
     * - recipient entries [0 .. size - 1] shift right to [1 .. size].
     * - recipient->array_[1].first gets middle_key (pulled down from parent).
     * - recipient->array_[0].second gets this node's last child (ValueAt(size - 1)).
     * - recipient size increases by 1.
     * - The borrowed child is reparented to recipient->GetPageId().
     * - This node size decreases by 1.
     *
     * @param recipient Right sibling internal page (underflowing).
     * @param middle_key Separator key pulled down from parent.
     * @param bpm Optional BufferPoolManager to update parent pointer of the moved child.
     */
    void MoveLastToFrontOf(BPlusTreeInternalPage *recipient, const KeyType &middle_key, BufferPoolManager *bpm = nullptr);

    /**
     * @brief Move first entry from this node to end of recipient (redistribution from right sibling).
     *
     * In an internal node:
     * - recipient->array_[size] gets {middle_key, this node's leftmost child ValueAt(0)}.
     * - recipient size increases by 1.
     * - The borrowed child is reparented to recipient->GetPageId().
     * - This node shifts its remaining entries left:
     *   array_[0].second becomes ValueAt(1).
     *   array_[1 .. size - 2] get array_[2 .. size - 1].
     * - This node size decreases by 1.
     *
     * @param recipient Left sibling internal page (underflowing).
     * @param middle_key Separator key pulled down from parent.
     * @param bpm Optional BufferPoolManager to update parent pointer of the moved child.
     */
    void MoveFirstToEndOf(BPlusTreeInternalPage *recipient, const KeyType &middle_key, BufferPoolManager *bpm = nullptr);

private:
    /**
     * @brief Universal 3-way comparator helper supporting both bool comp(a,b) and int comp(a,b).
     */
    template <typename Comp, typename K>
    static int Compare(const Comp &comp, const K &a, const K &b) {
        using RetType = std::invoke_result_t<Comp, K, K>;
        if constexpr (std::is_same_v<RetType, bool>) {
            if (comp(a, b)) return -1;
            if (comp(b, a)) return 1;
            return 0;
        } else {
            auto res = comp(a, b);
            if (res < 0) return -1;
            if (res > 0) return 1;
            return 0;
        }
    }
    MappingType array_[INTERNAL_PAGE_SIZE];
};

static_assert(sizeof(BPlusTreeInternalPage<int, page_id_t>) <= PAGE_SIZE,
              "BPlusTreeInternalPage size must fit within physical 4096-byte PAGE_SIZE");
