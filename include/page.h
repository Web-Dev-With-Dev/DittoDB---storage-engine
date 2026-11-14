#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>

#include "config.h"

/**
 * @class ReaderWriterLatch
 * @brief Portable, high-performance Reader-Writer latch with writer preference.
 *
 * Implemented using standard C++ std::mutex and std::condition_variable to guarantee
 * zero torn reads across all platforms (Linux GCC/Clang, Windows MSVC/MinGW) without
 * relying on buggy winpthreads or reader-starving POSIX rwlock implementations.
 */
class ReaderWriterLatch {
private:
    std::mutex mtx_;
    std::condition_variable cv_;
    int readers_{0};
    int writers_waiting_{0};
    bool writer_active_{false};

public:
    ReaderWriterLatch() = default;
    ~ReaderWriterLatch() = default;

    ReaderWriterLatch(const ReaderWriterLatch &) = delete;
    ReaderWriterLatch &operator=(const ReaderWriterLatch &) = delete;

    void LockExclusive() {
        std::unique_lock<std::mutex> lock(mtx_);
        ++writers_waiting_;
        cv_.wait(lock, [this] { return !writer_active_ && readers_ == 0; });
        --writers_waiting_;
        writer_active_ = true;
    }

    void UnlockExclusive() {
        std::unique_lock<std::mutex> lock(mtx_);
        writer_active_ = false;
        cv_.notify_all();
    }

    void LockShared() {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait(lock, [this] { return !writer_active_ && writers_waiting_ == 0; });
        ++readers_;
    }

    void UnlockShared() {
        std::unique_lock<std::mutex> lock(mtx_);
        --readers_;
        if (readers_ == 0) {
            cv_.notify_all();
        }
    }
};

/**
 * @class Page
 * @brief In-memory representation of a fixed-size 4096-byte database storage page.
 *
 * Each Page contains:
 * - A 4096-byte data payload buffer.
 * - Concurrency control metadata (pin count, dirty flag, page ID).
 * - Read/Write page-level latch (portable ReaderWriterLatch).
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
    mutable ReaderWriterLatch latch_;
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
 
    static constexpr size_t OFFSET_LSN = 4;

    /**
     * @brief Get the Log Sequence Number (LSN) from byte offset 4 of the page payload.
     * @return lsn_t The log sequence number.
     */
    lsn_t GetLSN() const;

    /**
     * @brief Set the Log Sequence Number (LSN) at byte offset 4 of the page payload.
     * @param lsn The log sequence number.
     */
    void SetLSN(lsn_t lsn);

    /**
     * @brief Zero-fill the 4096-byte page payload.
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