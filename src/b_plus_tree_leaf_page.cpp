#include "b_plus_tree_leaf_page.h"
#include "generic_key.h"
#include "rid.h"
#include <functional>

template <typename KeyType, typename ValueType, typename KeyComparator>
int BPlusTreeLeafPage<KeyType, ValueType, KeyComparator>::KeyIndex(const KeyType &key, const KeyComparator &comparator) const {
    int low = 0;
    int high = GetSize() - 1;
    while (low <= high) {
        int mid = low + (high - low) / 2;
        int cmp = Compare(comparator, array_[mid].first, key);
        if (cmp == 0) {
            return mid;
        }
        if (cmp < 0) {
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }
    return -1;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
int BPlusTreeLeafPage<KeyType, ValueType, KeyComparator>::LowerBound(const KeyType &key, const KeyComparator &comparator) const {
    int low = 0;
    int high = GetSize() - 1;
    int result = GetSize();
    while (low <= high) {
        int mid = low + (high - low) / 2;
        int cmp = Compare(comparator, array_[mid].first, key);
        if (cmp >= 0) {
            result = mid;
            high = mid - 1;
        } else {
            low = mid + 1;
        }
    }
    return result;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
bool BPlusTreeLeafPage<KeyType, ValueType, KeyComparator>::Lookup(const KeyType &key, ValueType *result, const KeyComparator &comparator) const {
    int index = KeyIndex(key, comparator);
    if (index != -1) {
        if (result != nullptr) {
            *result = array_[index].second;
        }
        return true;
    }
    return false;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
int BPlusTreeLeafPage<KeyType, ValueType, KeyComparator>::Insert(const KeyType &key, const ValueType &value, const KeyComparator &comparator) {
    int low = 0;
    int high = GetSize() - 1;
    int insert_idx = GetSize();

    while (low <= high) {
        int mid = low + (high - low) / 2;
        int cmp = Compare(comparator, array_[mid].first, key);
        if (cmp == 0) {
            return GetSize();
        }
        if (cmp < 0) {
            low = mid + 1;
        } else {
            insert_idx = mid;
            high = mid - 1;
        }
    }

    if (GetSize() >= GetMaxSize()) {
        return -1;
    }

    for (int i = GetSize(); i > insert_idx; --i) {
        array_[i] = array_[i - 1];
    }

    array_[insert_idx] = {key, value};
    IncreaseSize(1);
    return GetSize();
}

template <typename KeyType, typename ValueType, typename KeyComparator>
int BPlusTreeLeafPage<KeyType, ValueType, KeyComparator>::RemoveAndDeleteRecord(const KeyType &key, const KeyComparator &comparator) {
    int index = KeyIndex(key, comparator);
    if (index == -1) {
        return GetSize();
    }

    for (int i = index; i < GetSize() - 1; ++i) {
        array_[i] = array_[i + 1];
    }
    IncreaseSize(-1);
    return GetSize();
}

template <typename KeyType, typename ValueType, typename KeyComparator>
void BPlusTreeLeafPage<KeyType, ValueType, KeyComparator>::MoveHalfTo(BPlusTreeLeafPage *recipient) {
    assert(recipient != nullptr);
    int total_size = GetSize();
    int move_start = total_size / 2;
    int move_count = total_size - move_start;

    for (int i = 0; i < move_count; ++i) {
        recipient->array_[i] = array_[move_start + i];
    }

    recipient->SetSize(move_count);
    SetSize(move_start);

    recipient->SetNextPageId(GetNextPageId());
    SetNextPageId(recipient->GetPageId());
}

template <typename KeyType, typename ValueType, typename KeyComparator>
void BPlusTreeLeafPage<KeyType, ValueType, KeyComparator>::MoveAllTo(BPlusTreeLeafPage *recipient) {
    assert(recipient != nullptr);
    int target_start = recipient->GetSize();
    int move_count = GetSize();

    for (int i = 0; i < move_count; ++i) {
        recipient->array_[target_start + i] = array_[i];
    }

    recipient->IncreaseSize(move_count);
    recipient->SetNextPageId(GetNextPageId());
    SetSize(0);
    SetNextPageId(INVALID_PAGE_ID);
}

template <typename KeyType, typename ValueType, typename KeyComparator>
void BPlusTreeLeafPage<KeyType, ValueType, KeyComparator>::MoveFirstToEndOf(BPlusTreeLeafPage *recipient) {
    assert(recipient != nullptr);
    assert(GetSize() > 0);

    recipient->array_[recipient->GetSize()] = array_[0];
    recipient->IncreaseSize(1);

    for (int i = 0; i < GetSize() - 1; ++i) {
        array_[i] = array_[i + 1];
    }
    IncreaseSize(-1);
}

template <typename KeyType, typename ValueType, typename KeyComparator>
void BPlusTreeLeafPage<KeyType, ValueType, KeyComparator>::MoveLastToFrontOf(BPlusTreeLeafPage *recipient) {
    assert(recipient != nullptr);
    assert(GetSize() > 0);

    for (int i = recipient->GetSize(); i > 0; --i) {
        recipient->array_[i] = recipient->array_[i - 1];
    }

    recipient->array_[0] = array_[GetSize() - 1];
    recipient->IncreaseSize(1);
    IncreaseSize(-1);
}

// ==================== Explicit Template Instantiations ====================
template class BPlusTreeLeafPage<int, RID, std::less<int>>;
template class BPlusTreeLeafPage<int, RID, std::greater<int>>;
template class BPlusTreeLeafPage<GenericKey<4>, RID, GenericComparator<4>>;
template class BPlusTreeLeafPage<GenericKey<8>, RID, GenericComparator<8>>;
template class BPlusTreeLeafPage<GenericKey<16>, RID, GenericComparator<16>>;
template class BPlusTreeLeafPage<GenericKey<32>, RID, GenericComparator<32>>;
template class BPlusTreeLeafPage<GenericKey<64>, RID, GenericComparator<64>>;

