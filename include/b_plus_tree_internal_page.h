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
    int ValueIndex(const ValueType &value) const {
        for (int i = 0; i < GetSize(); ++i) {
            if (array_[i].second == value) {
                return i;
            }
        }
        return -1;
    }

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
    ValueType Lookup(const KeyType &key, const KeyComparator &comparator = KeyComparator{}) const {
        if (GetSize() <= 0) {
            return static_cast<ValueType>(INVALID_PAGE_ID);
        }
        if (GetSize() == 1) {
            return ValueAt(0);
        }

        int low = 1;
        int high = GetSize() - 1;
        int target_idx = 0; // Default to child pointer 0 if key < KeyAt(1)

        while (low <= high) {
            int mid = low + (high - low) / 2;
            if (Compare(comparator, KeyAt(mid), key) <= 0) {
                // KeyAt(mid) <= key: candidate child index found.
                // Continue searching to the right to find any higher valid split key.
                target_idx = mid;
                low = mid + 1;
            } else {
                // KeyAt(mid) > key: search to the left.
                high = mid - 1;
            }
        }

        return ValueAt(target_idx);
    }

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
    int InsertNodeAfter(const ValueType &old_value, const KeyType &new_key, const ValueType &new_value) {
        int index = ValueIndex(old_value);
        if (index == -1) {
            return -1;
        }

        int current_size = GetSize();
        assert(current_size < GetMaxSize());

        // Shift entries right to create space at index + 1
        for (int i = current_size; i > index + 1; --i) {
            array_[i] = array_[i - 1];
        }

        array_[index + 1].first = new_key;
        array_[index + 1].second = new_value;
        IncreaseSize(1);
        return GetSize();
    }

    // ==================== Node Management Helpers ====================

    /**
     * @brief Populate a newly created root node with two children and one separator key.
     * @param value0 Pointer to leftmost child.
     * @param key1 Separator key.
     * @param value1 Pointer to right child.
     */
    void Populate(const ValueType &value0, const KeyType &key1, const ValueType &value1) {
        SetValueAt(0, value0);
        SetKeyAt(1, key1);
        SetValueAt(1, value1);
        SetSize(2);
    }

    /**
     * @brief Remove an entry at a given index and shift remaining entries left.
     * @param index Index to remove.
     */
    void Remove(int index) {
        assert(index >= 0 && index < GetSize());
        for (int i = index; i < GetSize() - 1; ++i) {
            array_[i] = array_[i + 1];
        }
        IncreaseSize(-1);
    }

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
     * @tparam BPM Buffer pool manager type (templated to avoid circular header dependencies).
     * @param recipient Target sibling internal page.
     * @param bpm Optional BufferPoolManager to update parent pointers of all moved child pages.
     */
    template <typename BPM = void>
    void MoveHalfTo(BPlusTreeInternalPage *recipient, BPM *bpm = nullptr) {
        assert(recipient != nullptr);
        int total_size = GetSize();
        int move_start = total_size / 2;
        int move_count = total_size - move_start;

        for (int i = 0; i < move_count; ++i) {
            recipient->array_[i] = array_[move_start + i];
        }

        recipient->SetSize(move_count);
        SetSize(move_start);

        if constexpr (!std::is_same_v<BPM, void>) {
            if (bpm != nullptr) {
                for (int i = 0; i < move_count; ++i) {
                    page_id_t child_pid = recipient->ValueAt(i);
                    auto *child_frame = bpm->FetchPage(child_pid);
                    if (child_frame != nullptr) {
                        BPlusTreePage::Cast(child_frame)->SetParentPageId(recipient->GetPageId());
                        bpm->UnpinPage(child_pid, true);
                    }
                }
            }
        }
    }

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
