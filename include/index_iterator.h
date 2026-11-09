#pragma once

#include <cassert>
#include <cstddef>
#include <iterator>
#include <utility>

#include "config.h"
#include "page.h"
#include "buffer_pool_manager.h"
#include "b_plus_tree_leaf_page.h"

/**
 * @class IndexIterator
 * @brief Forward iterator that traverses the leaf chain of a B+ Tree sequentially.
 *
 * Traversal & Pin Management:
 * 1. Keeps the current leaf page pinned in the buffer pool manager while pointing to it,
 *    preventing eviction while the caller inspects elements via operator* or operator->.
 * 2. When advancing past the end of the current leaf (operator++):
 *    - Unpins the current leaf page frame.
 *    - Follows next_page_id to fetch the adjacent sibling leaf page.
 *    - If next_page_id is INVALID_PAGE_ID, transitions cleanly to End() (holding 0 pins).
 * 3. RAII Resource Safety:
 *    - Destructor automatically unpins any held leaf page frame.
 *    - Copy constructor and copy assignment increment the buffer pool frame pin count.
 *    - Move constructor and move assignment transfer frame pin ownership without leaks.
 *
 * @tparam KeyType Type of index key.
 * @tparam ValueType Type of stored value (defaults to RID).
 * @tparam KeyComparator Custom comparator type.
 */
template <typename KeyType, typename ValueType = RID, typename KeyComparator = std::less<KeyType>>
class IndexIterator {
public:
    using LeafPage = BPlusTreeLeafPage<KeyType, ValueType, KeyComparator>;
    using MappingType = typename LeafPage::MappingType;

    // Standard C++ iterator traits for STL compatibility (std::distance, range-for, etc.)
    using iterator_category = std::forward_iterator_tag;
    using value_type = MappingType;
    using difference_type = std::ptrdiff_t;
    using pointer = const MappingType *;
    using reference = const MappingType &;

    /**
     * @brief Construct an end-of-iteration sentinel iterator.
     */
    IndexIterator() : bpm_(nullptr), page_(nullptr), page_id_(INVALID_PAGE_ID), index_(0) {}

    /**
     * @brief Construct an iterator pointing to a specific index within a pinned leaf page.
     * @param bpm Pointer to BufferPoolManager.
     * @param page Pointer to currently pinned leaf page.
     * @param index Slot index within the leaf page (0 <= index <= size).
     */
    IndexIterator(BufferPoolManager *bpm, Page *page, int index = 0);

    /**
     * @brief Destructor unpins currently held leaf page to guarantee zero pin leaks.
     */
    ~IndexIterator();

    /**
     * @brief Copy constructor increments buffer pool pin count on the held page.
     */
    IndexIterator(const IndexIterator &other);

    /**
     * @brief Copy assignment operator with proper pin management.
     */
    IndexIterator &operator=(const IndexIterator &other);

    /**
     * @brief Move constructor transfers held page pin ownership without changing pin count.
     */
    IndexIterator(IndexIterator &&other) noexcept;

    /**
     * @brief Move assignment operator transfers held page pin ownership.
     */
    IndexIterator &operator=(IndexIterator &&other) noexcept;

    /**
     * @brief Check whether iterator has reached the end of the B+ Tree leaf chain.
     */
    bool IsEnd() const;

    /**
     * @brief Explicit boolean conversion checking if iterator currently points to a valid entry.
     */
    explicit operator bool() const;

    /**
     * @brief Dereference operator returning reference to current (key, value) pair.
     */
    const MappingType &operator*() const;

    /**
     * @brief Member access operator returning pointer to current (key, value) pair.
     */
    const MappingType *operator->() const;

    /**
     * @brief Prefix increment operator: advances to next entry across the leaf chain.
     * Safely no-ops if iterator has already reached End().
     */
    IndexIterator &operator++();

    /**
     * @brief Postfix increment operator.
     */
    IndexIterator operator++(int);

    /**
     * @brief Equality comparison operator.
     */
    bool operator==(const IndexIterator &other) const;

    /**
     * @brief Inequality comparison operator.
     */
    bool operator!=(const IndexIterator &other) const;

    // ==================== Diagnostic Accessors ====================
    int GetIndex() const { return index_; }
    page_id_t GetPageId() const { return page_id_; }
    Page *GetPage() const { return page_; }

    const KeyType &Key() const {
        return (*this)->first;
    }

    const ValueType &Value() const {
        return (*this)->second;
    }

private:
    /**
     * @brief Safely unpin the currently held page in BufferPoolManager.
     */
    void ReleaseCurrentPage();

    /**
     * @brief Advance through next_page_id pointers until a non-empty leaf is reached or leaf chain ends.
     */
    void AdvanceToNextValidLeaf();

    BufferPoolManager *bpm_{nullptr};
    Page *page_{nullptr};
    page_id_t page_id_{INVALID_PAGE_ID};
    int index_{0};
};
