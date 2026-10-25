#pragma once

#include <array>
#include <atomic>

#if defined(__unix__) || defined(__APPLE__)
#include <pthread.h>
#else
#include <shared_mutex>
#endif

#include "config.h"
#include "common_page_header.h"

/**
 * @class Page
 * @brief In-memory representation of a fixed-size 4096-byte database storage page.
 *
 * Each Page contains:
 * - A 4096-byte data payload buffer.
 * - Concurrency control metadata (pin count, dirty flag, page ID).
 * - Read/Write page-level latch (shared mutex on Windows / pthread_rwlock_t on POSIX).
 *
 * Latching vs Pinning:
 * - Pinning (Pin/Unpin): Prevents the BufferPoolManager from evicting the page while active.
 * - Latching (RLatch/WLatch): Protects the page's memory contents from concurrent read/write races.
 */
class Page {
private:
    std::atomic<page_id_t> page_id_;
    std::atomic<int> pin_count_;
    std::atomic<bool> is_dirty_;
#if defined(__unix__) || defined(__APPLE__)
    mutable pthread_rwlock_t latch_;
#else
    mutable std::shared_mutex latch_;
#endif
    std::array<char, PAGE_SIZE> data_;

public:
    /**
     * @brief Construct a new Page with default values (INVALID_PAGE_ID, pin_count 0, not dirty).
     */
    Page();

    /**
     * @brief Virtual destructor to support safe inheritance (e.g. TablePage).
     */
    virtual ~Page();

    // Disable copy semantics
    Page(const Page &) = delete;
    Page &operator=(const Page &) = delete;

    /**
     * @brief Get the unique page ID assigned to this page frame.
     * @return page_id_t Identifier of the stored page, or INVALID_PAGE_ID if unassigned.
     */
    page_id_t GetPageId() const;

    /**
     * @brief Get the current pin count of this page frame.
     * @return Number of concurrent threads/callers actively pinning this page.
     */
    int GetPinCount() const;

    /**
     * @brief Check whether this page has modified data that needs writing back to disk.
     * @return true if page is dirty, false otherwise.
     */
    bool IsDirty() const;

    /**
     * @brief Get a mutable pointer to the raw 4096-byte page payload.
     * @return char* Pointer to the page's data buffer.
     */
    char *GetData();

    /**
     * @brief Get an immutable pointer to the raw 4096-byte page payload.
     * @return const char* Pointer to the page's data buffer.
     */
    const char *GetData() const;
 
    /**
     * @brief Access the in-place CommonPageHeader located at byte offset 0 of the page payload.
     * @return CommonPageHeader* Pointer to in-place common page header.
     */
    CommonPageHeader *GetHeader();

    /**
     * @brief Access the const in-place CommonPageHeader located at byte offset 0 of the page payload.
     * @return const CommonPageHeader* Pointer to in-place common page header.
     */
    const CommonPageHeader *GetHeader() const;

    /**
     * @brief Get the Log Sequence Number (LSN) from the common page header.
     * @return lsn_t The log sequence number.
     */
    lsn_t GetLSN() const;

    /**
     * @brief Set the Log Sequence Number (LSN) on the common page header.
     * @param lsn The log sequence number.
     */
    void SetLSN(lsn_t lsn);

    /**
     * @brief Get the PageType discriminator from the common page header.
     * @return PageType The page discriminator.
     */
    PageType GetPageType() const;

    /**
     * @brief Set the PageType discriminator on the common page header.
     * @param type The page discriminator.
     */
    void SetPageType(PageType type);

    /**
     * @brief Zero-fill the 4096-byte page payload and reinitialize the CommonPageHeader with sentinels.
     */
    void ResetMemory();

    /**
     * @brief Set the page ID for this page frame.
     * @param id The logical page ID.
     */
    void SetPageId(page_id_t id);

    /**
     * @brief Set the dirty flag for this page.
     * @param dirty true if the page data has been modified.
     */
    void SetDirty(bool dirty);

    /**
     * @brief Increment the page's pin count by 1.
     */
    void Pin();

    /**
     * @brief Decrement the page's pin count by 1 (clamped to 0).
     */
    void Unpin();

    /**
     * @brief Acquire an exclusive write latch on this page.
     */
    void WLatch();

    /**
     * @brief Release the exclusive write latch on this page.
     */
    void WUnlatch();

    /**
     * @brief Acquire a shared read latch on this page.
     */
    void RLatch();

    /**
     * @brief Release the shared read latch on this page.
     */
    void RUnlatch();
};