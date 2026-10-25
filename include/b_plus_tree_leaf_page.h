#pragma once

#include <cstdint>
#include <cstddef>
#include <cassert>
#include <utility>
#include <functional>
#include <type_traits>
#include "config.h"
#include "rid.h"
#include "b_plus_tree_page.h"

/**
 * @class BPlusTreeLeafPage
 * @brief Store sorted (key, RID) pairs and a next_page_id pointer inside a fixed 4096-byte Page buffer.
 *
 * Leaf page physical memory layout inside a Page buffer:
 * -------------------------------------------------------------------------------------------------
 * | CommonPageHeader (24 Bytes)                                                                   |
 * | page_type (4B) | lsn (4B) | size (4B) | max_size (4B) | parent_page_id (4B) | page_id (4B)   |
 * -------------------------------------------------------------------------------------------------
 * | next_page_id_ (4 Bytes) | padding (4 Bytes alignment)                                         |
 * -------------------------------------------------------------------------------------------------
 * | array_[0] : (key 0, RID 0)                                                                    |
 * | array_[1] : (key 1, RID 1)                                                                    |
 * | array_[2] : (key 2, RID 2)                                                                    |
 * | ...                                                                                           |
 * | array_[size - 1] : (key size-1, RID size-1)                                                   |
 * -------------------------------------------------------------------------------------------------
 *
 * Invariants:
 * 1. Unlike Internal nodes, ALL entries from index 0 to size-1 are valid (key, value) pairs.
 * 2. Elements are strictly sorted in ascending order: KeyAt(0) < KeyAt(1) < ... < KeyAt(size-1).
 * 3. All leaf nodes are linked sequentially via next_page_id_ to support fast O(1) leaf-to-leaf range scans.
 */
template <typename KeyType, typename ValueType = RID, typename KeyComparator = std::less<KeyType>>
class BPlusTreeLeafPage : public BPlusTreePage {
public:
    using MappingType = std::pair<KeyType, ValueType>;

    static constexpr size_t RAW_HEADER_SIZE = sizeof(BPlusTreePage) + sizeof(page_id_t);
    // Align header boundary to 8 bytes for 64-bit keys/RIDs
    static constexpr size_t ALIGNED_HEADER_SIZE = ((RAW_HEADER_SIZE + 7) / 8) * 8; // 32 bytes
    static constexpr size_t LEAF_PAGE_SIZE = (PAGE_SIZE - ALIGNED_HEADER_SIZE) / sizeof(MappingType);

    BPlusTreeLeafPage() = default;

    /**
     * @brief Initialize a leaf page frame.
     * @param page_id The physical page ID assigned to this page.
     * @param parent_page_id Optional parent page ID (default: INVALID_PAGE_ID).
     * @param max_size Maximum key-value capacity (default: LEAF_PAGE_SIZE).
     */
    void Init(page_id_t page_id, page_id_t parent_page_id = INVALID_PAGE_ID, int max_size = static_cast<int>(LEAF_PAGE_SIZE)) {
        SetPageType(PageType::BPLUS_TREE_LEAF_PAGE);
        SetSize(0);
        SetMaxSize((max_size > 0 && max_size <= static_cast<int>(LEAF_PAGE_SIZE)) ? max_size : static_cast<int>(LEAF_PAGE_SIZE));
        SetParentPageId(parent_page_id);
        SetPageId(page_id);
        SetLSN(0);
        next_page_id_ = INVALID_PAGE_ID;
    }

    /**
     * @brief Reinterpret cast a raw Page pointer into a typed BPlusTreeLeafPage pointer.
     * @param page Raw memory Page pointer.
     * @return BPlusTreeLeafPage* In-place typed pointer.
     */
    static BPlusTreeLeafPage *Cast(Page *page) {
        if (page == nullptr) {
            return nullptr;
        }
        return reinterpret_cast<BPlusTreeLeafPage *>(page->GetData());
    }

    /**
     * @brief Reinterpret cast a const raw Page pointer into a const BPlusTreeLeafPage pointer.
     * @param page Const raw memory Page pointer.
     * @return const BPlusTreeLeafPage* In-place typed const pointer.
     */
    static const BPlusTreeLeafPage *Cast(const Page *page) {
        if (page == nullptr) {
            return nullptr;
        }
        return reinterpret_cast<const BPlusTreeLeafPage *>(page->GetData());
    }

    // ==================== Next Page Pointer (Linked Leaf Chain) ====================

    /**
     * @brief Get pointer to the next leaf page in the sequential scan chain.
     * @return page_id_t Next page ID or INVALID_PAGE_ID if this is the rightmost leaf.
     */
    page_id_t GetNextPageId() const {
        return next_page_id_;
    }

    /**
     * @brief Set pointer to the next leaf page in the sequential scan chain.
     * @param next_page_id Next leaf page ID.
     */
    void SetNextPageId(page_id_t next_page_id) {
        next_page_id_ = next_page_id;
    }

    // ==================== Element Accessors ====================

    /**
     * @brief Get key at index.
     * @param index Entry index (0 <= index < GetSize()).
     * @return const KeyType& Reference to key.
     */
    const KeyType &KeyAt(int index) const {
        assert(index >= 0 && index < GetSize());
        return array_[index].first;
    }

    /**
     * @brief Set key at index.
     * @param index Entry index.
     * @param key Key value.
     */
    void SetKeyAt(int index, const KeyType &key) {
        assert(index >= 0 && index < GetMaxSize());
        array_[index].first = key;
    }

    /**
     * @brief Get value (RID) at index.
     * @param index Entry index (0 <= index < GetSize()).
     * @return const ValueType& Reference to value.
     */
    const ValueType &ValueAt(int index) const {
        assert(index >= 0 && index < GetSize());
        return array_[index].second;
    }

    /**
     * @brief Set value (RID) at index.
     * @param index Entry index.
     * @param value Value.
     */
    void SetValueAt(int index, const ValueType &value) {
        assert(index >= 0 && index < GetMaxSize());
        array_[index].second = value;
    }

    /**
     * @brief Get entire (key, value) pair at index.
     * @param index Entry index.
     * @return const MappingType& Reference to pair.
     */
    const MappingType &ItemAt(int index) const {
        assert(index >= 0 && index < GetSize());
        return array_[index];
    }

    /**
     * @brief Find exact index of a key via binary search.
     * @param key Key to search for.
     * @param comparator Optional comparator.
     * @return int Index if found, or -1 if absent.
     */
    int KeyIndex(const KeyType &key, const KeyComparator &comparator = KeyComparator{}) const {
        int low = 0;
        int high = GetSize() - 1;
        while (low <= high) {
            int mid = low + (high - low) / 2;
            int cmp = Compare(comparator, array_[mid].first, key);
            if (cmp == 0) {
                return mid;
            }
            if (cmp < 0) {
                low = mid + 1;
            } else {
                high = mid - 1;
            }
        }
        return -1;
    }

    // ==================== Binary-Search Lookup ====================

    /**
     * @brief Look up the value associated with an exact key using binary search.
     * @param key Search key.
     * @param[out] result Optional output pointer to store the retrieved value (RID).
     * @param comparator Optional comparator.
     * @return true if key found, false otherwise.
     */
    bool Lookup(const KeyType &key, ValueType *result = nullptr, const KeyComparator &comparator = KeyComparator{}) const {
        int index = KeyIndex(key, comparator);
        if (index != -1) {
            if (result != nullptr) {
                *result = array_[index].second;
            }
            return true;
        }
        return false;
    }

    // ==================== Sorted Insert ====================

    /**
     * @brief Insert a (key, value) pair into the leaf page maintaining sorted key order.
     *
     * Performs a binary search for the insertion point:
     * - If key already exists (duplicate), rejects insertion and returns current size.
     * - If space permits, shifts subsequent entries right and inserts at the sorted position.
     *
     * @param key Key to insert.
     * @param value Associated value (RID).
     * @param comparator Optional custom comparator.
     * @return int The updated size of the leaf page, or -1 if the page is full.
     */
    int Insert(const KeyType &key, const ValueType &value, const KeyComparator &comparator = KeyComparator{}) {
        int low = 0;
        int high = GetSize() - 1;
        int insert_idx = GetSize(); // Default to append at end

        while (low <= high) {
            int mid = low + (high - low) / 2;
            int cmp = Compare(comparator, array_[mid].first, key);
            if (cmp == 0) {
                // Duplicate key: do not insert duplicate in unique index
                return GetSize();
            }
            if (cmp < 0) {
                low = mid + 1;
            } else {
                insert_idx = mid;
                high = mid - 1;
            }
        }

        if (GetSize() >= GetMaxSize()) {
            return -1; // Page full, split required
        }

        // Shift elements right to open slot at insert_idx
        for (int i = GetSize(); i > insert_idx; --i) {
            array_[i] = array_[i - 1];
        }

        array_[insert_idx] = {key, value};
        IncreaseSize(1);
        return GetSize();
    }

    // ==================== Record Removal ====================

    /**
     * @brief Remove record associated with key and maintain sorted order.
     * @param key Key to delete.
     * @param comparator Optional comparator.
     * @return int Updated size if deleted, or current size if key not found.
     */
    int RemoveAndDeleteRecord(const KeyType &key, const KeyComparator &comparator = KeyComparator{}) {
        int index = KeyIndex(key, comparator);
        if (index == -1) {
            return GetSize();
        }

        for (int i = index; i < GetSize() - 1; ++i) {
            array_[i] = array_[i + 1];
        }
        IncreaseSize(-1);
        return GetSize();
    }

    // ==================== Split & Merge Redistribution Helpers ====================

    /**
     * @brief Move upper half of entries from this node to recipient during node split.
     * Updates linked list pointers between this node and recipient.
     * @param recipient Pointer to newly allocated sibling leaf page.
     */
    void MoveHalfTo(BPlusTreeLeafPage *recipient) {
        assert(recipient != nullptr);
        int total_size = GetSize();
        int move_start = total_size / 2;
        int move_count = total_size - move_start;

        for (int i = 0; i < move_count; ++i) {
            recipient->array_[i] = array_[move_start + i];
        }

        recipient->SetSize(move_count);
        SetSize(move_start);

        // Update sequential chain: this -> recipient -> old_next
        recipient->SetNextPageId(GetNextPageId());
        SetNextPageId(recipient->GetPageId());
    }

    /**
     * @brief Move all entries from this node into recipient during node merge.
     * @param recipient Target left sibling leaf page.
     */
    void MoveAllTo(BPlusTreeLeafPage *recipient) {
        assert(recipient != nullptr);
        int target_start = recipient->GetSize();
        int move_count = GetSize();

        for (int i = 0; i < move_count; ++i) {
            recipient->array_[target_start + i] = array_[i];
        }

        recipient->IncreaseSize(move_count);
        recipient->SetNextPageId(GetNextPageId());
        SetSize(0);
        SetNextPageId(INVALID_PAGE_ID);
    }

    /**
     * @brief Move first entry from this node to end of recipient (redistribution from right sibling).
     * @param recipient Left sibling leaf node.
     */
    void MoveFirstToEndOf(BPlusTreeLeafPage *recipient) {
        assert(recipient != nullptr);
        assert(GetSize() > 0);

        recipient->array_[recipient->GetSize()] = array_[0];
        recipient->IncreaseSize(1);

        for (int i = 0; i < GetSize() - 1; ++i) {
            array_[i] = array_[i + 1];
        }
        IncreaseSize(-1);
    }

    /**
     * @brief Move last entry from this node to front of recipient (redistribution from left sibling).
     * @param recipient Right sibling leaf node.
     */
    void MoveLastToFrontOf(BPlusTreeLeafPage *recipient) {
        assert(recipient != nullptr);
        assert(GetSize() > 0);

        for (int i = recipient->GetSize(); i > 0; --i) {
            recipient->array_[i] = recipient->array_[i - 1];
        }

        recipient->array_[0] = array_[GetSize() - 1];
        recipient->IncreaseSize(1);
        IncreaseSize(-1);
    }

    /**
     * @brief Get pointer to the underlying sorted entry array.
     */
    const MappingType *GetArray() const {
        return array_;
    }

private:
    /**
     * @brief Universal 3-way comparator helper supporting both bool comp(a,b) and int comp(a,b).
     */
    template <typename Comp, typename K>
    static int Compare(const Comp &comp, const K &a, const K &b) {
        if constexpr (std::is_invocable_r_v<bool, Comp, K, K>) {
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

    page_id_t next_page_id_{INVALID_PAGE_ID};
    MappingType array_[LEAF_PAGE_SIZE];
};

static_assert(sizeof(BPlusTreeLeafPage<int, RID>) <= PAGE_SIZE,
              "BPlusTreeLeafPage size must fit within physical 4096-byte PAGE_SIZE");
