#pragma once

#include <cstdint>
#include <cstddef>
#include <utility>
#include <vector>
#include "config.h"

/**
 * @brief Bucket page for Extendible Hash Table.
 *
 * Stored inside a 4096-byte Page in BufferPoolManager.
 * Stores key-value entries up to BUCKET_ARRAY_SIZE.
 */
template <typename KeyType, typename ValueType>
class ExtendibleHTableBucketPage {
public:
    static constexpr size_t BUCKET_HEADER_SIZE = 16;
    static constexpr size_t ENTRY_SIZE = sizeof(std::pair<KeyType, ValueType>);
    static constexpr size_t BUCKET_ARRAY_SIZE = (PAGE_SIZE - BUCKET_HEADER_SIZE) / ENTRY_SIZE;

    ExtendibleHTableBucketPage() = default;

    /**
     * @brief Initialize a bucket page.
     * @param page_id The physical page ID assigned to this bucket.
     * @param max_size Maximum key-value capacity of this bucket (default: BUCKET_ARRAY_SIZE).
     */
    void Init(page_id_t page_id, uint32_t max_size = BUCKET_ARRAY_SIZE);

    /**
     * @brief Look up a key in the bucket.
     * @param key Key to search for.
     * @param result Pointer to store the value if found.
     * @return true if key found, false otherwise.
     */
    bool Lookup(const KeyType &key, ValueType *result) const;

    /**
     * @brief Insert or update a key-value pair.
     * @param key Key to insert.
     * @param value Value to insert.
     * @return true if inserted or updated, false if bucket is full and key does not exist.
     */
    bool Insert(const KeyType &key, const ValueType &value);

    /**
     * @brief Remove a key-value pair from the bucket.
     * Replaces removed entry with the last entry for O(1) removal.
     * @param key Key to remove.
     * @return true if found and removed, false otherwise.
     */
    bool Remove(const KeyType &key);

    /**
     * @brief Check whether the bucket has reached its maximum entry capacity.
     * @return true if full, false otherwise.
     */
    bool IsFull() const { return size_ >= max_size_; }

    /**
     * @brief Check whether the bucket contains zero entries.
     * @return true if empty, false otherwise.
     */
    bool IsEmpty() const { return size_ == 0; }

    /**
     * @brief Get current number of stored key-value entries.
     * @return uint32_t Number of entries.
     */
    uint32_t Size() const { return size_; }

    /**
     * @brief Get maximum number of entries this bucket can hold.
     * @return uint32_t Maximum entry capacity.
     */
    uint32_t MaxSize() const { return max_size_; }

    /**
     * @brief Get const reference to the key at index idx.
     * @param idx Entry index.
     * @return Const reference to key.
     */
    const KeyType &KeyAt(uint32_t idx) const { return entries_[idx].first; }

    /**
     * @brief Get const reference to the value at index idx.
     * @param idx Entry index.
     * @return Const reference to value.
     */
    const ValueType &ValueAt(uint32_t idx) const { return entries_[idx].second; }

    /**
     * @brief Get const reference to the key-value pair at index idx.
     * @param idx Entry index.
     * @return Const reference to pair.
     */
    const std::pair<KeyType, ValueType> &EntryAt(uint32_t idx) const { return entries_[idx]; }

    /**
     * @brief Clear all entries in the bucket and destroy stored objects.
     */
    void Clear();

    /**
     * @brief Get the physical page ID of this bucket.
     * @return page_id_t Bucket page ID.
     */
    page_id_t GetPageId() const { return page_id_; }

    /**
     * @brief Set the physical page ID of this bucket.
     * @param page_id Bucket page ID.
     */
    void SetPageId(page_id_t page_id) { page_id_ = page_id; }

private:
    page_id_t page_id_{INVALID_PAGE_ID};
    uint32_t size_{0};
    uint32_t max_size_{BUCKET_ARRAY_SIZE};
    uint32_t reserved_{0};
    std::pair<KeyType, ValueType> entries_[BUCKET_ARRAY_SIZE];
};

static_assert(sizeof(ExtendibleHTableBucketPage<int64_t, int64_t>) <= PAGE_SIZE,
              "ExtendibleHTableBucketPage must fit within PAGE_SIZE");
