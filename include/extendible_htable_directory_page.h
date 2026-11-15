#pragma once

#include <cstdint>
#include <cstddef>
#include "config.h"

/**
 * @brief Directory page for Extendible Hash Table.
 *
 * Stored inside a 4096-byte Page in BufferPoolManager.
 * Manages global depth, local depths, and page pointers to bucket pages.
 *
 * Page layout:
 * -----------------------------------------------------------------------------
 * | PageId (4B) | LSN (4B) | GlobalDepth (4B) | MaxDepth (4B)                |
 * -----------------------------------------------------------------------------
 * | LocalDepths (512B) [1 byte per directory entry]                           |
 * -----------------------------------------------------------------------------
 * | BucketPageIds (2048B) [4 bytes per directory entry (page_id_t)]           |
 * -----------------------------------------------------------------------------
 * | Padding / Unused space (up to PAGE_SIZE = 4096B)                          |
 * -----------------------------------------------------------------------------
 */
class ExtendibleHTableDirectoryPage {
public:
    static constexpr size_t HTABLE_DIRECTORY_MAX_DEPTH = 9;
    static constexpr size_t HTABLE_DIRECTORY_ARRAY_SIZE = 1u << HTABLE_DIRECTORY_MAX_DEPTH; // 512 entries

    ExtendibleHTableDirectoryPage() = default;

    /**
     * @brief Initialize directory page with initial depth 0 and initial bucket page.
     * @param page_id The physical page_id assigned to this directory.
     * @param initial_bucket_page_id The initial bucket page ID.
     * @param max_depth Maximum global depth supported (default 9).
     */
    void Init(page_id_t page_id, page_id_t initial_bucket_page_id, uint32_t max_depth = HTABLE_DIRECTORY_MAX_DEPTH);

    // ==================== Depth & Index Operations ====================

    /**
     * @brief Compute directory index from hash value.
     * @param hash Computed hash value.
     * @return uint32_t Bucket index in the directory.
     */
    uint32_t HashToBucketIndex(size_t hash) const;

    /**
     * @brief Get the split image index for a given bucket index.
     * Toggles the bit at position (local_depth - 1).
     * @param bucket_idx Directory index to compute split image for.
     * @return uint32_t Split image directory index.
     */
    uint32_t GetSplitImageIndex(uint32_t bucket_idx) const;

    /**
     * @brief Returns bitmask for the current global depth (lowest global_depth bits set to 1).
     * @return uint32_t Global depth bitmask.
     */
    uint32_t GetGlobalDepthMask() const;

    /**
     * @brief Returns bitmask for the local depth of a given bucket.
     * @param bucket_idx Target bucket index.
     * @return uint32_t Local depth bitmask.
     */
    uint32_t GetLocalDepthMask(uint32_t bucket_idx) const;

    /**
     * @brief Get current global depth of the directory.
     * @return uint32_t Global depth.
     */
    uint32_t GetGlobalDepth() const { return global_depth_; }

    /**
     * @brief Set global depth of the directory (clamped to max_depth_).
     * @param global_depth New global depth.
     */
    void SetGlobalDepth(uint32_t global_depth);

    /**
     * @brief Increment global depth and double directory entries.
     */
    void IncrGlobalDepth();

    /**
     * @brief Decrement global depth if all local depths are strictly less than global depth.
     */
    void DecrGlobalDepth();

    /**
     * @brief Check whether directory can be safely shrunk.
     * @return true if all local depths < global_depth_, false otherwise.
     */
    bool CanShrink() const;

    /**
     * @brief Get maximum supported global depth.
     * @return uint32_t Maximum depth.
     */
    uint32_t GetMaxDepth() const { return max_depth_; }

    /**
     * @brief Get the number of active directory entries (2^global_depth).
     * @return uint32_t Number of active entries.
     */
    uint32_t Size() const { return 1u << global_depth_; }

    // ==================== Local Depths & Bucket IDs ====================

    /**
     * @brief Get the physical page ID of the bucket assigned to directory index.
     * @param bucket_idx Directory entry index.
     * @return page_id_t Bucket page ID, or INVALID_PAGE_ID if index out of bounds.
     */
    page_id_t GetBucketPageId(uint32_t bucket_idx) const;

    /**
     * @brief Set the bucket page ID for a directory entry.
     * @param bucket_idx Directory entry index.
     * @param bucket_page_id Target bucket physical page ID.
     */
    void SetBucketPageId(uint32_t bucket_idx, page_id_t bucket_page_id);

    /**
     * @brief Get the local depth of a bucket entry.
     * @param bucket_idx Directory entry index.
     * @return uint8_t Local depth.
     */
    uint8_t GetLocalDepth(uint32_t bucket_idx) const;

    /**
     * @brief Set the local depth of a bucket entry.
     * @param bucket_idx Directory entry index.
     * @param local_depth New local depth value.
     */
    void SetLocalDepth(uint32_t bucket_idx, uint8_t local_depth);

    /**
     * @brief Increment the local depth of a bucket entry.
     * @param bucket_idx Directory entry index.
     */
    void IncrLocalDepth(uint32_t bucket_idx);

    /**
     * @brief Get physical page ID of this directory page.
     * @return page_id_t Directory page ID.
     */
    page_id_t GetPageId() const { return page_id_; }

    /**
     * @brief Set physical page ID of this directory page.
     * @param page_id Directory page ID.
     */
    void SetPageId(page_id_t page_id) { page_id_ = page_id; }

    /**
     * @brief Get Log Sequence Number.
     * @return uint32_t LSN.
     */
    uint32_t GetLsn() const { return lsn_; }

    /**
     * @brief Set Log Sequence Number.
     * @param lsn New LSN.
     */
    void SetLsn(uint32_t lsn) { lsn_ = lsn; }

    /**
     * @brief Verifies integrity of directory entries and local depths.
     * Ensures all local depths <= global depth <= max depth.
     * @return true if valid, false otherwise.
     */
    bool VerifyIntegrity() const;

private:
    page_id_t page_id_{INVALID_PAGE_ID};
    uint32_t lsn_{0};
    uint32_t global_depth_{0};
    uint32_t max_depth_{HTABLE_DIRECTORY_MAX_DEPTH};

    uint8_t local_depths_[HTABLE_DIRECTORY_ARRAY_SIZE];
    page_id_t bucket_page_ids_[HTABLE_DIRECTORY_ARRAY_SIZE];
};

static_assert(sizeof(ExtendibleHTableDirectoryPage) <= PAGE_SIZE,
              "ExtendibleHTableDirectoryPage must fit within PAGE_SIZE (4096 bytes)");
