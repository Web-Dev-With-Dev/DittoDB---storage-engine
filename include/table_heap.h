#pragma once

#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "buffer_pool_manager.h"
#include "config.h"
#include "rid.h"
#include "tuple.h"

class TableHeap;

/**
 * @brief Forward iterator that traverses all live tuples across an entire TableHeap
 * (across all linked pages, automatically skipping deleted/tombstoned slots).
 */
class TableIterator {
private:
    const TableHeap *table_heap_{nullptr};
    RID rid_{};
    Tuple tuple_{};

    void AdvanceToNextLiveTuple();

public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = Tuple;
    using difference_type = std::ptrdiff_t;
    using pointer = const Tuple *;
    using reference = const Tuple &;

    /**
     * @brief Construct an end-of-iteration sentinel iterator.
     */
    TableIterator() = default;

    /**
     * @brief Construct an iterator starting at a specific RID within the TableHeap.
     * @param table_heap Pointer to TableHeap.
     * @param rid Initial record identifier to begin scanning from.
     */
    TableIterator(const TableHeap *table_heap, const RID &rid);

    reference operator*() const { return tuple_; }
    pointer operator->() const { return &tuple_; }

    TableIterator &operator++();
    TableIterator operator++(int);

    bool operator==(const TableIterator &other) const;
    bool operator!=(const TableIterator &other) const { return !(*this == other); }

    /**
     * @brief Get the current tuple value.
     * @return Current tuple.
     */
    Tuple GetTuple() const { return tuple_; }

    /**
     * @brief Get the RID of the current live tuple.
     * @return RID record identifier.
     */
    RID GetRid() const { return rid_; }

    /**
     * @brief Check whether iterator has reached the end of the table heap.
     * @return true if finished, false otherwise.
     */
    bool IsEnd() const { return !rid_.IsValid(); }

    /**
     * @brief Check whether iterator currently points to an active tuple.
     * @return true if valid tuple available.
     */
    bool HasNext() const { return !IsEnd(); }

    /**
     * @brief Explicit boolean conversion checking if more tuples remain.
     */
    explicit operator bool() const { return !IsEnd(); }

    /**
     * @brief Retrieve current item and advance iterator forward.
     * @param[out] tuple Optional output pointer for tuple.
     * @param[out] rid Optional output pointer for RID.
     * @return true if successful, false if already at end.
     */
    bool Next(Tuple *tuple = nullptr, RID *rid = nullptr);
};

/**
 * @class TableHeap
 * @brief Represents a physical relational table stored as a doubly-linked list
 * of TablePages in the BufferPoolManager.
 *
 * It manages cross-page allocation, sequential insertion, tuple fetching,
 * cross-page migration updates (Delete Old + Insert New), and table-level iteration.
 */
class TableHeap {
private:
    BufferPoolManager *bpm_{nullptr};
    std::shared_ptr<BufferPoolManager> bpm_sp_{nullptr};
    page_id_t first_page_id_{INVALID_PAGE_ID};
    mutable std::mutex latch_;

public:
    using Iterator = TableIterator;
    using const_iterator = TableIterator;
    using iterator = TableIterator;

    /**
     * @brief Create or attach to a TableHeap using raw BufferPoolManager pointer.
     * @param bpm The buffer pool manager.
     * @param first_page_id The root page ID of the table heap. If INVALID_PAGE_ID,
     *                      a new page is automatically allocated and initialized.
     */
    explicit TableHeap(BufferPoolManager *bpm, page_id_t first_page_id = INVALID_PAGE_ID);

    /**
     * @brief Create or attach to a TableHeap using shared pointer to BufferPoolManager.
     * @param bpm The buffer pool manager shared pointer.
     * @param first_page_id The root page ID of the table heap. If INVALID_PAGE_ID,
     *                      a new page is automatically allocated and initialized.
     */
    explicit TableHeap(std::shared_ptr<BufferPoolManager> bpm, page_id_t first_page_id = INVALID_PAGE_ID);

    ~TableHeap() = default;

    // Disable copy semantics
    TableHeap(const TableHeap &) = delete;
    TableHeap &operator=(const TableHeap &) = delete;

    // Enable move semantics
    TableHeap(TableHeap &&) noexcept = default;
    TableHeap &operator=(TableHeap &&) noexcept = default;

    // ==================== Tuple CRUD Operations ====================

    /**
     * @brief Insert a tuple into the table heap.
     * Searches for space starting from the first page, following next_page links.
     * If all existing pages are full, a new page is allocated and linked.
     * @param tuple Tuple to insert.
     * @param[out] rid Output pointer to receive assigned RID.
     * @return true on success, false if insertion failed.
     */
    bool InsertTuple(const Tuple &tuple, RID *rid);

    /**
     * @brief Read a tuple by its RID.
     * @param rid Target record identifier.
     * @param[out] tuple Output pointer to receive tuple.
     * @return true if found and active, false otherwise.
     */
    bool GetTuple(const RID &rid, Tuple *tuple) const;

    /**
     * @brief Read a tuple by its RID as std::optional.
     * @param rid Target record identifier.
     * @return Optional containing tuple if active, std::nullopt otherwise.
     */
    std::optional<Tuple> GetTuple(const RID &rid) const;

    /**
     * @brief Update a tuple in the table heap.
     * 1. Attempts an in-place / in-page update on the current page.
     * 2. If the updated tuple cannot fit on the current page (even after defragmentation),
     *    it executes Strategy 3 (Delete Old + Insert New):
     *    - Marks the old tuple deleted: table_page->MarkDelete(old_rid).
     *    - Inserts the new tuple on an existing or newly allocated page in the heap.
     *    - Sets new_rid to the new location (for secondary index pointer updates).
     *    - If insertion fails, rolls back the delete on the old page.
     * @param new_tuple New tuple data.
     * @param rid RID of tuple to update.
     * @param[out] new_rid Optional output pointer to receive updated RID if migration occurred.
     * @return true on success, false otherwise.
     */
    bool UpdateTuple(const Tuple &new_tuple, const RID &rid, RID *new_rid = nullptr);

    /**
     * @brief Update a tuple in the table heap with retrieval of prior tuple content.
     * @param new_tuple New tuple data.
     * @param[out] old_tuple Output pointer receiving previous tuple content.
     * @param rid RID of tuple to update.
     * @param[out] new_rid Optional output pointer to receive updated RID if migration occurred.
     * @return true on success, false otherwise.
     */
    bool UpdateTuple(const Tuple &new_tuple, Tuple *old_tuple, const RID &rid, RID *new_rid = nullptr);

    /**
     * @brief Mark a tuple as deleted (soft delete via tombstone flag).
     * @param rid RID of tuple to mark deleted.
     * @return true on success, false otherwise.
     */
    bool MarkDelete(const RID &rid);

    /**
     * @brief Apply a delete (physically clears slot offset and size for slot reuse).
     * @param rid RID of tuple to clear.
     * @return true on success, false otherwise.
     */
    bool ApplyDelete(const RID &rid);

    /**
     * @brief Rollback a marked delete (restores soft-deleted tuple).
     * @param rid RID of tuple to restore.
     * @return true on success, false otherwise.
     */
    bool RollbackDelete(const RID &rid);

    // ==================== Iteration over Entire Table ====================

    /**
     * @brief Get iterator pointing to the first live tuple in the table heap.
     */
    Iterator begin();
    Iterator end();
    Iterator begin() const;
    Iterator end() const;
    Iterator cbegin() const;
    Iterator cend() const;

    Iterator Begin() { return begin(); }
    Iterator End() { return end(); }
    Iterator Begin() const { return begin(); }
    Iterator End() const { return end(); }

    // ==================== Diagnostics & Metadata ====================

    /**
     * @brief Get the root page ID of this table heap.
     * @return page_id_t First page ID.
     */
    page_id_t GetFirstPageId() const { return first_page_id_; }

    /**
     * @brief Get pointer to the underlying BufferPoolManager.
     * @return BufferPoolManager pointer.
     */
    BufferPoolManager *GetBufferPoolManager() const { return bpm_; }

    /**
     * @brief Traverse the page list and return all page IDs belonging to this table heap.
     * @return Vector of page IDs in traversal order.
     */
    std::vector<page_id_t> GetPageIds() const;

    /**
     * @brief Count total number of live (non-deleted) tuples currently stored across all pages.
     * @return Total active tuple count.
     */
    size_t GetTupleCount() const;
};
