#include "b_plus_tree_internal_page.h"
#include "buffer_pool_manager.h"
#include "generic_key.h"
#include <functional>

template <typename KeyType, typename ValueType, typename KeyComparator>
int BPlusTreeInternalPage<KeyType, ValueType, KeyComparator>::ValueIndex(const ValueType &value) const {
    for (int i = 0; i < GetSize(); ++i) {
        if (array_[i].second == value) {
            return i;
        }
    }
    return -1;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
ValueType BPlusTreeInternalPage<KeyType, ValueType, KeyComparator>::Lookup(const KeyType &key, const KeyComparator &comparator) const {
    if (GetSize() <= 0) {
        return static_cast<ValueType>(INVALID_PAGE_ID);
    }
    if (GetSize() == 1) {
        return ValueAt(0);
    }

    int low = 1;
    int high = GetSize() - 1;
    int target_idx = 0;

    while (low <= high) {
        int mid = low + (high - low) / 2;
        if (Compare(comparator, KeyAt(mid), key) <= 0) {
            target_idx = mid;
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }

    return ValueAt(target_idx);
}

template <typename KeyType, typename ValueType, typename KeyComparator>
int BPlusTreeInternalPage<KeyType, ValueType, KeyComparator>::InsertNodeAfter(const ValueType &old_value, const KeyType &new_key, const ValueType &new_value) {
    int index = ValueIndex(old_value);
    if (index == -1) {
        return -1;
    }

    int current_size = GetSize();
    assert(current_size < GetMaxSize());

    for (int i = current_size; i > index + 1; --i) {
        array_[i] = array_[i - 1];
    }

    array_[index + 1].first = new_key;
    array_[index + 1].second = new_value;
    IncreaseSize(1);
    return GetSize();
}

template <typename KeyType, typename ValueType, typename KeyComparator>
void BPlusTreeInternalPage<KeyType, ValueType, KeyComparator>::Populate(const ValueType &value0, const KeyType &key1, const ValueType &value1) {
    SetValueAt(0, value0);
    SetKeyAt(1, key1);
    SetValueAt(1, value1);
    SetSize(2);
}

template <typename KeyType, typename ValueType, typename KeyComparator>
void BPlusTreeInternalPage<KeyType, ValueType, KeyComparator>::Remove(int index) {
    assert(index >= 0 && index < GetSize());
    for (int i = index; i < GetSize() - 1; ++i) {
        array_[i] = array_[i + 1];
    }
    IncreaseSize(-1);
}

template <typename KeyType, typename ValueType, typename KeyComparator>
void BPlusTreeInternalPage<KeyType, ValueType, KeyComparator>::MoveHalfTo(BPlusTreeInternalPage *recipient, BufferPoolManager *bpm) {
    assert(recipient != nullptr);
    int total_size = GetSize();
    int move_start = total_size / 2;
    int move_count = total_size - move_start;

    for (int i = 0; i < move_count; ++i) {
        recipient->array_[i] = array_[move_start + i];
    }

    recipient->SetSize(move_count);
    SetSize(move_start);

    if (bpm != nullptr) {
        for (int i = 0; i < move_count; ++i) {
            page_id_t child_pid = recipient->ValueAt(i);
            auto *child_frame = bpm->FetchPage(child_pid);
            if (child_frame != nullptr) {
                BPlusTreePage::Cast(child_frame)->SetParentPageId(recipient->GetPageId());
                bpm->UnpinPage(child_pid, true);
            }
        }
    }
}

template <typename KeyType, typename ValueType, typename KeyComparator>
void BPlusTreeInternalPage<KeyType, ValueType, KeyComparator>::MoveAllTo(BPlusTreeInternalPage *recipient, const KeyType &middle_key, BufferPoolManager *bpm) {
    assert(recipient != nullptr);
    int target_start = recipient->GetSize();
    int move_count = GetSize();

    recipient->array_[target_start] = {middle_key, array_[0].second};

    for (int i = 1; i < move_count; ++i) {
        recipient->array_[target_start + i] = array_[i];
    }

    recipient->IncreaseSize(move_count);
    SetSize(0);

    if (bpm != nullptr) {
        for (int i = 0; i < move_count; ++i) {
            page_id_t child_pid = recipient->ValueAt(target_start + i);
            auto *child_frame = bpm->FetchPage(child_pid);
            if (child_frame != nullptr) {
                BPlusTreePage::Cast(child_frame)->SetParentPageId(recipient->GetPageId());
                bpm->UnpinPage(child_pid, true);
            }
        }
    }
}

template <typename KeyType, typename ValueType, typename KeyComparator>
void BPlusTreeInternalPage<KeyType, ValueType, KeyComparator>::MoveLastToFrontOf(BPlusTreeInternalPage *recipient, const KeyType &middle_key, BufferPoolManager *bpm) {
    assert(recipient != nullptr);
    assert(GetSize() > 0);

    int rec_size = recipient->GetSize();
    for (int i = rec_size; i > 0; --i) {
        recipient->array_[i] = recipient->array_[i - 1];
    }

    recipient->array_[1].first = middle_key;
    page_id_t borrowed_child_pid = ValueAt(GetSize() - 1);
    recipient->array_[0].second = borrowed_child_pid;
    recipient->IncreaseSize(1);
    IncreaseSize(-1);

    if (bpm != nullptr) {
        auto *child_frame = bpm->FetchPage(borrowed_child_pid);
        if (child_frame != nullptr) {
            BPlusTreePage::Cast(child_frame)->SetParentPageId(recipient->GetPageId());
            bpm->UnpinPage(borrowed_child_pid, true);
        }
    }
}

template <typename KeyType, typename ValueType, typename KeyComparator>
void BPlusTreeInternalPage<KeyType, ValueType, KeyComparator>::MoveFirstToEndOf(BPlusTreeInternalPage *recipient, const KeyType &middle_key, BufferPoolManager *bpm) {
    assert(recipient != nullptr);
    assert(GetSize() > 0);

    int rec_size = recipient->GetSize();
    page_id_t borrowed_child_pid = ValueAt(0);
    recipient->array_[rec_size] = {middle_key, borrowed_child_pid};
    recipient->IncreaseSize(1);

    array_[0].second = ValueAt(1);
    for (int i = 1; i < GetSize() - 1; ++i) {
        array_[i] = array_[i + 1];
    }
    IncreaseSize(-1);

    if (bpm != nullptr) {
        auto *child_frame = bpm->FetchPage(borrowed_child_pid);
        if (child_frame != nullptr) {
            BPlusTreePage::Cast(child_frame)->SetParentPageId(recipient->GetPageId());
            bpm->UnpinPage(borrowed_child_pid, true);
        }
    }
}

// ==================== Explicit Template Instantiations ====================
template class BPlusTreeInternalPage<int, page_id_t, std::less<int>>;
template class BPlusTreeInternalPage<int, page_id_t, std::greater<int>>;
template class BPlusTreeInternalPage<GenericKey<4>, page_id_t, GenericComparator<4>>;
template class BPlusTreeInternalPage<GenericKey<8>, page_id_t, GenericComparator<8>>;
template class BPlusTreeInternalPage<GenericKey<16>, page_id_t, GenericComparator<16>>;
template class BPlusTreeInternalPage<GenericKey<32>, page_id_t, GenericComparator<32>>;
template class BPlusTreeInternalPage<GenericKey<64>, page_id_t, GenericComparator<64>>;

