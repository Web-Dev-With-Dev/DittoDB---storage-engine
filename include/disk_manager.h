#pragma once

#include <atomic>
#include <cstddef>
#include <fstream>
#include <memory>
#include <string>

#include "config.h"

/**
 * @class DiskManager
 * @brief Manages the physical persistence and reading/writing of database pages on disk.
 *
 * Responsibilities:
 * - Allocates new physical pages sequentially on disk.
 * - Reads pages from disk storage into raw in-memory buffers (4096-byte blocks).
 * - Writes pages from in-memory buffers to disk storage.
 * - Synchronizes concurrent I/O and page allocation per database file via a shared state registry.
 */
class DiskManager {
private:
    /// Per-file shared state (mutex + next_page_id) shared across all instances pointing to the same file
    struct FileState;

    static std::shared_ptr<FileState> GetFileState(const std::string &file_name);

    std::fstream db_file_;
    std::string file_name_;
    std::shared_ptr<FileState> file_state_;
    std::atomic<size_t> num_reads_{0};
    std::atomic<size_t> num_writes_{0};

public:
    /**
     * @brief Construct a new DiskManager for a specified database file path.
     * Opens existing file or creates a new one if it does not exist.
     * Initializes the shared per-file page allocation cursor based on current file size
     * if this is the first active instance for the file.
     * @param file_name Path to the database file on disk.
     * @throws std::runtime_error If file creation, opening, or size determination fails,
     *                            or if file size is not a multiple of PAGE_SIZE.
     */
    explicit DiskManager(const std::string &file_name);

    DiskManager(const DiskManager &) = delete;
    DiskManager &operator=(const DiskManager &) = delete;

    /**
     * @brief Allocate a new sequential page on disk.
     * Atomically increments and returns the next available page ID.
     * @return The newly allocated page ID.
     */
    page_id_t AllocatePage();

    /**
     * @brief Deallocate a page on disk.
     * In an append-only/page-slotted storage architecture, this acts as a logical deallocation hook
     * (or placeholder for physical free-list/bitmap reclaiming).
     * @param page_id Logical page ID to deallocate.
     */
    void DeallocatePage(page_id_t page_id);

    /**
     * @brief Non-mutating peek at the next page ID to be allocated.
     * @return The next available page ID without incrementing the cursor.
     */
    page_id_t GetNextPageId() const;

    /**
     * @brief Write a 4096-byte page from memory to the specified disk location.
     * Zero-fills intermediate pages if writing beyond current end-of-file.
     * Flushes changes to disk.
     * @param page_id Logical page ID determining the file offset (page_id * PAGE_SIZE).
     * @param data Pointer to the in-memory buffer containing page data.
     * @throws std::runtime_error If seek, write, or flush fails.
     */
    void WritePage(page_id_t page_id, const char *data);

    /**
     * @brief Read a 4096-byte page from disk into an in-memory buffer.
     * If reading past end-of-file, missing bytes are zero-filled.
     * @param page_id Logical page ID to read from disk.
     * @param buffer Pointer to the destination memory buffer (must hold at least PAGE_SIZE bytes).
     * @throws std::runtime_error If seeking to the target offset fails.
     */
    void ReadPage(page_id_t page_id, char *buffer);

    /**
     * @brief Get total number of physical disk page reads executed.
     */
    size_t GetNumReads() const { return num_reads_.load(std::memory_order_relaxed); }

    /**
     * @brief Get total number of physical disk page writes executed.
     */
    size_t GetNumWrites() const { return num_writes_.load(std::memory_order_relaxed); }

    /**
     * @brief Reset physical disk read and write I/O counters to zero.
     */
    void ResetIOCounters() {
        num_reads_.store(0, std::memory_order_relaxed);
        num_writes_.store(0, std::memory_order_relaxed);
    }

    /**
     * @brief Close the underlying database file stream safely.
     */
    ~DiskManager();
};
