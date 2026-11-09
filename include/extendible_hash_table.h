#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <vector>

#include "buffer_pool_manager.h"
#include "config.h"
#include "extendible_htable_bucket_page.h"
#include "extendible_htable_directory_page.h"

/**
 * @brief Extendible Hash Table implementation backed by BufferPoolManager.
 *
 * Demonstrates:
 * - Dynamic directory page management in buffer frames
 * - Bucket page allocation, splitting, and key redistribution
 * - Directory doubling (IncrGlobalDepth) and shrinking (DecrGlobalDepth)
 * - Safe page pinning and dirty flag tracking in BufferPoolManager
 */
template <typename KeyType, typename ValueType, typename HashFunc = std::hash<KeyType>>
class ExtendibleHashTable {
public:
    using BucketPage = ExtendibleHTableBucketPage<KeyType, ValueType>;

    /**
     * @brief Construct a new ExtendibleHashTable.
     * Allocates an initial DirectoryPage and initial BucketPage in the BufferPoolManager.
     * @param name Diagnostic identifier name.
     * @param bpm Pointer to BufferPoolManager.
     * @param bucket_max_size Optional custom capacity per bucket (default: max fit in 4096B).
     * @param hash_fn Hash function to use.
     */
    ExtendibleHashTable(const std::string &name, BufferPoolManager *bpm,
                        uint32_t bucket_max_size = 0, HashFunc hash_fn = HashFunc{});

    /**
     * @brief Open an existing ExtendibleHashTable from an existing directory page on disk.
     */
    static ExtendibleHashTable Open(const std::string &name, BufferPoolManager *bpm,
                                    page_id_t directory_page_id, uint32_t bucket_max_size = 0,
                                    HashFunc hash_fn = HashFunc{});

private:
    ExtendibleHashTable() = default;

public:
    ExtendibleHashTable(ExtendibleHashTable &&) noexcept = default;
    ExtendibleHashTable &operator=(ExtendibleHashTable &&) noexcept = default;
    ExtendibleHashTable(const ExtendibleHashTable &) = delete;
    ExtendibleHashTable &operator=(const ExtendibleHashTable &) = delete;

    ~ExtendibleHashTable() = default;

    /**
     * @brief Insert or update a key-value pair.
     * Automatically handles bucket splitting and directory expansion when a bucket overflows.
     * @param key Key to insert.
     * @param value Value to associate with key.
     * @return true on success, false if table has reached maximum allowable capacity.
     */
    bool Insert(const KeyType &key, const ValueType &value);

    /**
     * @brief Lookup the value associated with a key.
     * @param key Key to find.
     * @param result Pointer to store the value.
     * @return true if key found, false otherwise.
     */
    bool GetValue(const KeyType &key, ValueType *result) const;

    /**
     * @brief Remove a key from the hash table.
     * @param key Key to remove.
     * @return true if removed, false if key did not exist.
     */
    bool Remove(const KeyType &key);

    // ==================== Inspection & Metrics ====================

    /**
     * @brief Get current global depth of the hash table directory.
     * @return uint32_t Global depth.
     */
    uint32_t GetGlobalDepth() const;

    /**
     * @brief Get local depth of a specific bucket entry.
     * @param bucket_idx Directory index.
     * @return uint32_t Local depth.
     */
    uint32_t GetLocalDepth(uint32_t bucket_idx) const;

    /**
     * @brief Get physical page ID of the bucket assigned to a directory index.
     * @param bucket_idx Directory index.
     * @return page_id_t Bucket page ID, or INVALID_PAGE_ID if fetch fails.
     */
    page_id_t GetBucketPageId(uint32_t bucket_idx) const;

    /**
     * @brief Count total number of unique physical bucket pages referenced by directory.
     * @return size_t Count of unique bucket pages.
     */
    size_t GetNumUniqueBuckets() const;

    /**
     * @brief Get physical page ID of the directory page.
     * @return page_id_t Directory page ID.
     */
    page_id_t GetDirectoryPageId() const { return directory_page_id_; }

    /**
     * @brief Get table diagnostic identifier name.
     * @return const std::string& Table name.
     */
    const std::string &GetName() const { return name_; }

    /**
     * @brief Check whether internal directory depth and bucket invariants are valid.
     * @return true if valid, false otherwise.
     */
    bool VerifyIntegrity() const;

private:
    std::string name_;
    BufferPoolManager *bpm_{nullptr};
    page_id_t directory_page_id_{INVALID_PAGE_ID};
    uint32_t bucket_max_size_{0};
    HashFunc hash_fn_{};
    mutable std::unique_ptr<std::shared_mutex> rwlock_{std::make_unique<std::shared_mutex>()};
};
