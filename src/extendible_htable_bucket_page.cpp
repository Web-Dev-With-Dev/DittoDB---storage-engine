#include "extendible_htable_bucket_page.h"
#include "rid.h"
#include <string>

template <typename KeyType, typename ValueType>
void ExtendibleHTableBucketPage<KeyType, ValueType>::Init(page_id_t page_id, uint32_t max_size) {
    page_id_ = page_id;
    size_ = 0;
    max_size_ = (max_size > 0 && max_size <= BUCKET_ARRAY_SIZE) ? max_size : static_cast<uint32_t>(BUCKET_ARRAY_SIZE);
    reserved_ = 0;
}

template <typename KeyType, typename ValueType>
bool ExtendibleHTableBucketPage<KeyType, ValueType>::Lookup(const KeyType &key, ValueType *result) const {
    for (uint32_t i = 0; i < size_; ++i) {
        if (entries_[i].first == key) {
            if (result) {
                *result = entries_[i].second;
            }
            return true;
        }
    }
    return false;
}

template <typename KeyType, typename ValueType>
bool ExtendibleHTableBucketPage<KeyType, ValueType>::Insert(const KeyType &key, const ValueType &value) {
    for (uint32_t i = 0; i < size_; ++i) {
        if (entries_[i].first == key) {
            entries_[i].second = value;
            return true;
        }
    }
    if (IsFull()) {
        return false;
    }
    new (&entries_[size_]) std::pair<KeyType, ValueType>(key, value);
    ++size_;
    return true;
}

template <typename KeyType, typename ValueType>
bool ExtendibleHTableBucketPage<KeyType, ValueType>::Remove(const KeyType &key) {
    for (uint32_t i = 0; i < size_; ++i) {
        if (entries_[i].first == key) {
            if (i + 1 < size_) {
                entries_[i] = std::move(entries_[size_ - 1]);
            }
            entries_[size_ - 1].~pair();
            --size_;
            return true;
        }
    }
    return false;
}

template <typename KeyType, typename ValueType>
void ExtendibleHTableBucketPage<KeyType, ValueType>::Clear() {
    for (uint32_t i = 0; i < size_; ++i) {
        entries_[i].~pair();
    }
    size_ = 0;
}

// ==================== Explicit Template Instantiations ====================
template class ExtendibleHTableBucketPage<int, int>;
template class ExtendibleHTableBucketPage<int, std::string>;
template class ExtendibleHTableBucketPage<int, RID>;
template class ExtendibleHTableBucketPage<std::string, int>;
template class ExtendibleHTableBucketPage<std::string, std::string>;
template class ExtendibleHTableBucketPage<int64_t, int64_t>;

