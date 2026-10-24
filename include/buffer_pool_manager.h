#pragma once

#include <condition_variable>
#include <cstddef>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "config.h"
#include "disk_manager.h"
#include "lru_k_replacer.h"
#include "page.h"

/**
 * @class BufferPoolManager
 * @brief Manages caching of physical database pages between memory frames and disk storage.
 *
 * Responsibilities:
 * - Coordinates page fetching, allocation, and eviction using an LRU-K replacer.
 * - Maintains a page table mapping logical page_id_t to in-memory frame_id_t.
 * - Manages dirty page write-backs and page unpinning.
 * - Guarantees thread-safe metadata management via an internal mutex latch.
 */
class BufferPoolManager {
public:
    /**
     * @brief Construct a new BufferPoolManager instance.
     * @param pool_size Maximum number of pages that can be held in memory.
     * @param disk_manager Raw pointer to the underlying DiskManager.
     * @param k The k parameter for the LRU-K replacer policy (default: 2).
     * @throws std::invalid_argument If pool_size == 0 or disk_manager == nullptr.
     */
    BufferPoolManager(size_t pool_size, DiskManager *disk_manager, size_t k = 2);

    /**
     * @brief Construct a new BufferPoolManager instance with shared ownership of DiskManager.
     * @param pool_size Maximum number of pages that can be held in memory.
     * @param disk_manager Shared pointer to the underlying DiskManager.
     * @param k The k parameter for the LRU-K replacer policy (default: 2).
     * @throws std::invalid_argument If pool_size == 0 or disk_manager == nullptr.
     */
    BufferPoolManager(size_t pool_size, std::shared_ptr<DiskManager> disk_manager, size_t k = 2);

    /**
     * @brief Destructor flushes all dirty pages to disk before shutting down.
     */
    ~BufferPoolManager();

    // Disable copy and move semantics
    BufferPoolManager(const BufferPoolManager &) = delete;
    BufferPoolManager &operator=(const BufferPoolManager &) = delete;
    BufferPoolManager(BufferPoolManager &&) = delete;
    BufferPoolManager &operator=(BufferPoolManager &&) = delete;

    /**
     * @brief Fetch the requested page from the buffer pool.
     * 1. Check if the page is already in the buffer pool (page table hit).
     *    If found, pin it, record access, and return.
     * 2. If page miss, find an available frame via free_list_ or replacer_->Evict()
     *    (writing back dirty victim page to disk if needed).
     * 3. Read the page from disk into the freed frame.
     * 4. Update the page table, pin the page, record access, and return the Page pointer.
     * @param page_id Logical ID of the page to fetch.
     * @return Pointer to the Page object, or nullptr if page cannot be fetched (e.g. invalid ID or all frames pinned).
     */
    Page *FetchPage(page_id_t page_id);

    /**
     * @brief Unpin a page in the buffer pool, decrementing its pin count.
     * @param page_id Logical ID of the page to unpin.
     * @param is_dirty true if the caller modified page contents, false otherwise.
     * @return true if page was successfully unpinned, false if page is not found or pin count was already 0.
     */
    bool UnpinPage(page_id_t page_id, bool is_dirty);

    /**
     * @brief Allocate a new page on disk and bring it into the buffer pool.
     * Finds an available frame, allocates a new page ID via DiskManager, zeroes the frame memory,
     * pins the page, and assigns the allocated page ID.
     * @param[out] page_id Output pointer where the newly assigned page ID is stored (set to INVALID_PAGE_ID on failure).
     * @return Pointer to the newly allocated Page object, or nullptr if all frames are pinned.
     */
    Page *NewPage(page_id_t *page_id);

    /**
     * @brief Delete a page from the buffer pool and free its frame.
     * @param page_id Logical ID of the page to delete.
     * @return true if page was deleted or was not present; false if page is currently pinned.
     */
    bool DeletePage(page_id_t page_id);

    /**
     * @brief Flush a specific page to disk if it is currently in the buffer pool and dirty.
     * @param page_id Logical ID of the page to flush.
     * @return true if page was flushed or not dirty; false if page is not in the buffer pool or invalid.
     */
    bool FlushPage(page_id_t page_id);

    /**
     * @brief Flush all dirty pages currently cached in the buffer pool to disk.
     */
    void FlushAllPages();

    // ==================== Diagnostics & Inspection ====================

    /**
     * @brief Get the total buffer pool capacity (number of frames).
     * @return Maximum number of pages that can be held simultaneously.
     */
    size_t GetPoolSize() const { return pool_size_; }

    /**
     * @brief Get the number of unallocated frames currently in the free list.
     * @return Free list size.
     */
    size_t GetFreeListSize() const;

    /**
     * @brief Check whether a specific page is currently cached in the buffer pool.
     * @param page_id Logical page ID to query.
     * @return true if present in the page table, false otherwise.
     */
    bool IsPageInPool(page_id_t page_id) const;

    /**
     * @brief Look up the frame index currently caching a given page ID.
     * @param page_id Logical page ID.
     * @return Frame index (frame_id_t) if cached, or INVALID_FRAME_ID if not present.
     */
    frame_id_t GetFrameId(page_id_t page_id) const;

    /**
     * @brief Get a raw pointer to the underlying contiguous array of Page frames.
     * @return Pointer to Page array.
     */
    Page *GetPages() { return pages_.get(); }
    const Page *GetPages() const { return pages_.get(); }

    /**
     * @brief Access a specific Page by its physical frame index.
     * @param frame_id Frame index (0 <= frame_id < pool_size_).
     * @return Pointer to the Page frame, or nullptr if frame_id is out of bounds.
     */
    Page *GetPage(frame_id_t frame_id);
    const Page *GetPage(frame_id_t frame_id) const;

private:
    /**
     * @brief Find an available frame from the free list or via LRU-K eviction.
     * @param[out] frame_id Output pointer to receive the acquired frame index.
     * @param lock Reference to the active unique_lock on latch_ (may be unlocked during dirty victim flush).
     * @return true if an available frame was found; false if all frames are pinned.
     */
    bool FindAvailableFrame(frame_id_t *frame_id, std::unique_lock<std::mutex> &lock);
    bool FindAvailableFrame(frame_id_t *frame_id);

    size_t pool_size_;
    DiskManager *disk_manager_{nullptr};
    std::shared_ptr<DiskManager> disk_manager_sp_{nullptr};
    std::unique_ptr<LRUKReplacer> replacer_;

    // In-memory contiguous frame array
    std::unique_ptr<Page[]> pages_;

    // Lookup: page_id -> frame_id
    std::unordered_map<page_id_t, frame_id_t> page_table_;

    // Unassigned frames
    std::list<frame_id_t> free_list_;

    // Concurrency control for BufferPoolManager metadata
    mutable std::mutex latch_;

    // Frame reservation & per-frame I/O synchronization (PostgreSQL / InnoDB model)
    std::vector<bool> frame_io_in_progress_;
    std::condition_variable io_cv_;
};

