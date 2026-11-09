#include "extendible_hash_table.h"
#include "rid.h"
#include <cassert>
#include <string>

template <typename KeyType, typename ValueType, typename HashFunc>
ExtendibleHashTable<KeyType, ValueType, HashFunc>::ExtendibleHashTable(const std::string &name,
                                                                       BufferPoolManager *bpm,
                                                                       uint32_t bucket_max_size,
                                                                       HashFunc hash_fn)
    : name_(name), bpm_(bpm), bucket_max_size_(bucket_max_size), hash_fn_(hash_fn) {
    // 1. Allocate directory page
    Page *dir_page = bpm_->NewPage(&directory_page_id_);
    assert(dir_page != nullptr && "Failed to allocate directory page for ExtendibleHashTable");

    // 2. Allocate initial bucket page
    page_id_t initial_bucket_id = INVALID_PAGE_ID;
    Page *bucket_page = bpm_->NewPage(&initial_bucket_id);
    assert(bucket_page != nullptr && "Failed to allocate initial bucket page");

    // 3. Initialize bucket page
    auto *bucket = reinterpret_cast<BucketPage *>(bucket_page->GetData());
    bucket->Init(initial_bucket_id, bucket_max_size_);

    // 4. Initialize directory page with initial bucket
    auto *dir = reinterpret_cast<ExtendibleHTableDirectoryPage *>(dir_page->GetData());
    dir->Init(directory_page_id_, initial_bucket_id);

    // 5. Unpin both pages as dirty
    bpm_->UnpinPage(initial_bucket_id, true);
    bpm_->UnpinPage(directory_page_id_, true);
}

template <typename KeyType, typename ValueType, typename HashFunc>
ExtendibleHashTable<KeyType, ValueType, HashFunc>
ExtendibleHashTable<KeyType, ValueType, HashFunc>::Open(const std::string &name, BufferPoolManager *bpm,
                                                        page_id_t directory_page_id, uint32_t bucket_max_size,
                                                        HashFunc hash_fn) {
    ExtendibleHashTable table;
    table.name_ = name;
    table.bpm_ = bpm;
    table.directory_page_id_ = directory_page_id;
    table.bucket_max_size_ = bucket_max_size;
    table.hash_fn_ = hash_fn;
    return table;
}

template <typename KeyType, typename ValueType, typename HashFunc>
bool ExtendibleHashTable<KeyType, ValueType, HashFunc>::Insert(const KeyType &key, const ValueType &value) {
    std::unique_lock<std::shared_mutex> lock(*rwlock_);

    while (true) {
        Page *dir_page = bpm_->FetchPage(directory_page_id_);
        if (!dir_page) return false;
        auto *dir = reinterpret_cast<ExtendibleHTableDirectoryPage *>(dir_page->GetData());

        size_t hash = hash_fn_(key);
        uint32_t bucket_idx = dir->HashToBucketIndex(hash);
        page_id_t bucket_page_id = dir->GetBucketPageId(bucket_idx);

        Page *b_page = bpm_->FetchPage(bucket_page_id);
        if (!b_page) {
            bpm_->UnpinPage(directory_page_id_, false);
            return false;
        }
        auto *bucket = reinterpret_cast<BucketPage *>(b_page->GetData());

        // Case 1: Key already exists or bucket has free space
        if (bucket->Insert(key, value)) {
            bpm_->UnpinPage(bucket_page_id, true);
            bpm_->UnpinPage(directory_page_id_, false);
            return true;
        }

        // Case 2: Bucket is full and key does not exist -> Bucket Split needed!
        uint32_t local_depth = dir->GetLocalDepth(bucket_idx);
        uint32_t global_depth = dir->GetGlobalDepth();

        if (local_depth >= dir->GetMaxDepth()) {
            // Table reached maximum supported depth
            bpm_->UnpinPage(bucket_page_id, false);
            bpm_->UnpinPage(directory_page_id_, false);
            return false;
        }

        // If local depth equals global depth, double the directory
        if (local_depth == global_depth) {
            dir->IncrGlobalDepth();
            global_depth = dir->GetGlobalDepth();
        }

        // Allocate a new bucket page
        page_id_t new_bucket_page_id = INVALID_PAGE_ID;
        Page *new_b_page = bpm_->NewPage(&new_bucket_page_id);
        if (!new_b_page) {
            bpm_->UnpinPage(bucket_page_id, false);
            bpm_->UnpinPage(directory_page_id_, true);
            return false;
        }
        auto *new_bucket = reinterpret_cast<BucketPage *>(new_b_page->GetData());
        new_bucket->Init(new_bucket_page_id, bucket_max_size_);

        // Update directory entries: indices matching (old_local_depth)-th bit point to new bucket
        uint32_t new_ld = local_depth + 1;
        uint32_t split_mask = 1u << local_depth;
        uint32_t dir_size = dir->Size();

        for (uint32_t i = 0; i < dir_size; ++i) {
            if (dir->GetBucketPageId(i) == bucket_page_id) {
                dir->SetLocalDepth(i, static_cast<uint8_t>(new_ld));
                if ((i & split_mask) != 0) {
                    dir->SetBucketPageId(i, new_bucket_page_id);
                }
            }
        }

        // Redistribute existing keys from old bucket into old or new bucket
        std::vector<std::pair<KeyType, ValueType>> old_entries;
        old_entries.reserve(bucket->Size());
        for (uint32_t i = 0; i < bucket->Size(); ++i) {
            old_entries.push_back(bucket->EntryAt(i));
        }
        bucket->Clear();

        for (const auto &entry : old_entries) {
            size_t h = hash_fn_(entry.first);
            uint32_t target_idx = dir->HashToBucketIndex(h);
            if (dir->GetBucketPageId(target_idx) == new_bucket_page_id) {
                new_bucket->Insert(entry.first, entry.second);
            } else {
                bucket->Insert(entry.first, entry.second);
            }
        }

        // Unpin pages
        bpm_->UnpinPage(new_bucket_page_id, true);
        bpm_->UnpinPage(bucket_page_id, true);
        bpm_->UnpinPage(directory_page_id_, true);

        // Loop continues and retries insertion
    }
}

template <typename KeyType, typename ValueType, typename HashFunc>
bool ExtendibleHashTable<KeyType, ValueType, HashFunc>::GetValue(const KeyType &key, ValueType *result) const {
    std::shared_lock<std::shared_mutex> lock(*rwlock_);

    Page *dir_page = bpm_->FetchPage(directory_page_id_);
    if (!dir_page) return false;
    auto *dir = reinterpret_cast<ExtendibleHTableDirectoryPage *>(dir_page->GetData());

    size_t hash = hash_fn_(key);
    uint32_t bucket_idx = dir->HashToBucketIndex(hash);
    page_id_t bucket_page_id = dir->GetBucketPageId(bucket_idx);

    bpm_->UnpinPage(directory_page_id_, false);

    Page *b_page = bpm_->FetchPage(bucket_page_id);
    if (!b_page) return false;
    auto *bucket = reinterpret_cast<BucketPage *>(b_page->GetData());

    bool found = bucket->Lookup(key, result);
    bpm_->UnpinPage(bucket_page_id, false);
    return found;
}

template <typename KeyType, typename ValueType, typename HashFunc>
bool ExtendibleHashTable<KeyType, ValueType, HashFunc>::Remove(const KeyType &key) {
    std::unique_lock<std::shared_mutex> lock(*rwlock_);

    Page *dir_page = bpm_->FetchPage(directory_page_id_);
    if (!dir_page) return false;
    auto *dir = reinterpret_cast<ExtendibleHTableDirectoryPage *>(dir_page->GetData());

    size_t hash = hash_fn_(key);
    uint32_t bucket_idx = dir->HashToBucketIndex(hash);
    page_id_t bucket_page_id = dir->GetBucketPageId(bucket_idx);

    Page *b_page = bpm_->FetchPage(bucket_page_id);
    if (!b_page) {
        bpm_->UnpinPage(directory_page_id_, false);
        return false;
    }
    auto *bucket = reinterpret_cast<BucketPage *>(b_page->GetData());

    bool removed = bucket->Remove(key);
    bpm_->UnpinPage(bucket_page_id, removed);

    // Check if directory can shrink
    if (dir->CanShrink()) {
        dir->DecrGlobalDepth();
        bpm_->UnpinPage(directory_page_id_, true);
    } else {
        bpm_->UnpinPage(directory_page_id_, false);
    }

    return removed;
}

template <typename KeyType, typename ValueType, typename HashFunc>
uint32_t ExtendibleHashTable<KeyType, ValueType, HashFunc>::GetGlobalDepth() const {
    std::shared_lock<std::shared_mutex> lock(*rwlock_);
    Page *dir_page = bpm_->FetchPage(directory_page_id_);
    if (!dir_page) return 0;
    auto *dir = reinterpret_cast<ExtendibleHTableDirectoryPage *>(dir_page->GetData());
    uint32_t gd = dir->GetGlobalDepth();
    bpm_->UnpinPage(directory_page_id_, false);
    return gd;
}

template <typename KeyType, typename ValueType, typename HashFunc>
uint32_t ExtendibleHashTable<KeyType, ValueType, HashFunc>::GetLocalDepth(uint32_t bucket_idx) const {
    std::shared_lock<std::shared_mutex> lock(*rwlock_);
    Page *dir_page = bpm_->FetchPage(directory_page_id_);
    if (!dir_page) return 0;
    auto *dir = reinterpret_cast<ExtendibleHTableDirectoryPage *>(dir_page->GetData());
    uint32_t ld = dir->GetLocalDepth(bucket_idx);
    bpm_->UnpinPage(directory_page_id_, false);
    return ld;
}

template <typename KeyType, typename ValueType, typename HashFunc>
page_id_t ExtendibleHashTable<KeyType, ValueType, HashFunc>::GetBucketPageId(uint32_t bucket_idx) const {
    std::shared_lock<std::shared_mutex> lock(*rwlock_);
    Page *dir_page = bpm_->FetchPage(directory_page_id_);
    if (!dir_page) return INVALID_PAGE_ID;
    auto *dir = reinterpret_cast<ExtendibleHTableDirectoryPage *>(dir_page->GetData());
    page_id_t id = dir->GetBucketPageId(bucket_idx);
    bpm_->UnpinPage(directory_page_id_, false);
    return id;
}

template <typename KeyType, typename ValueType, typename HashFunc>
size_t ExtendibleHashTable<KeyType, ValueType, HashFunc>::GetNumUniqueBuckets() const {
    std::shared_lock<std::shared_mutex> lock(*rwlock_);
    Page *dir_page = bpm_->FetchPage(directory_page_id_);
    if (!dir_page) return 0;
    auto *dir = reinterpret_cast<ExtendibleHTableDirectoryPage *>(dir_page->GetData());
    std::set<page_id_t> unique_pages;
    uint32_t sz = dir->Size();
    for (uint32_t i = 0; i < sz; ++i) {
        unique_pages.insert(dir->GetBucketPageId(i));
    }
    bpm_->UnpinPage(directory_page_id_, false);
    return unique_pages.size();
}

template <typename KeyType, typename ValueType, typename HashFunc>
bool ExtendibleHashTable<KeyType, ValueType, HashFunc>::VerifyIntegrity() const {
    std::shared_lock<std::shared_mutex> lock(*rwlock_);
    Page *dir_page = bpm_->FetchPage(directory_page_id_);
    if (!dir_page) return false;
    auto *dir = reinterpret_cast<ExtendibleHTableDirectoryPage *>(dir_page->GetData());
    bool valid = dir->VerifyIntegrity();
    bpm_->UnpinPage(directory_page_id_, false);
    return valid;
}

// ==================== Explicit Template Instantiations ====================
template class ExtendibleHashTable<int, int>;
template class ExtendibleHashTable<int, std::string>;
template class ExtendibleHashTable<int, RID>;
template class ExtendibleHashTable<std::string, int>;
template class ExtendibleHashTable<std::string, std::string>;

