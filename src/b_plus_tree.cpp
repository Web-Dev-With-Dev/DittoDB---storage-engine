#include "b_plus_tree.h"
#include <fstream>
#include <sstream>
#include <queue>
#include <set>
#include <unordered_set>
#include <optional>

template <typename KeyType, typename ValueType, typename KeyComparator>
BPlusTree<KeyType, ValueType, KeyComparator>::BPlusTree(std::string name, BufferPoolManager *bpm,
                                                        const KeyComparator &comparator,
                                                        int leaf_max_size, int internal_max_size)
    : name_(std::move(name)), bpm_(bpm), comparator_(comparator) {
    leaf_max_size_ = (leaf_max_size > 0) ? leaf_max_size : static_cast<int>(LeafPage::LEAF_PAGE_SIZE);
    internal_max_size_ = (internal_max_size > 0) ? internal_max_size : static_cast<int>(InternalPage::INTERNAL_PAGE_SIZE);

    if (bpm_ != nullptr) {
        Page *header_frame = bpm_->NewPage(&header_page_id_);
        if (header_frame != nullptr) {
            auto *header = BPlusTreeHeaderPage::Cast(header_frame);
            header->Init(header_page_id_, INVALID_PAGE_ID, sizeof(KeyType));
            bpm_->UnpinPage(header_page_id_, true);
        }
    }
}

template <typename KeyType, typename ValueType, typename KeyComparator>
BPlusTree<KeyType, ValueType, KeyComparator>::BPlusTree(std::string name, page_id_t header_page_id,
                                                        BufferPoolManager *bpm,
                                                        const KeyComparator &comparator,
                                                        int leaf_max_size, int internal_max_size)
    : name_(std::move(name)), bpm_(bpm), header_page_id_(header_page_id), comparator_(comparator) {
    leaf_max_size_ = (leaf_max_size > 0) ? leaf_max_size : static_cast<int>(LeafPage::LEAF_PAGE_SIZE);
    internal_max_size_ = (internal_max_size > 0) ? internal_max_size : static_cast<int>(InternalPage::INTERNAL_PAGE_SIZE);

    if (header_page_id_ != INVALID_PAGE_ID && bpm_ != nullptr) {
        Page *header_frame = bpm_->FetchPage(header_page_id_);
        if (header_frame != nullptr) {
            const auto *header = BPlusTreeHeaderPage::Cast(header_frame);
            root_page_id_ = header->GetRootPageId();
            bpm_->UnpinPage(header_page_id_, false);
        }
    }
}

template <typename KeyType, typename ValueType, typename KeyComparator>
page_id_t BPlusTree<KeyType, ValueType, KeyComparator>::GetRootPageId() const {
    if (header_page_id_ != INVALID_PAGE_ID && bpm_ != nullptr) {
        Page *header_frame = bpm_->FetchPage(header_page_id_);
        if (header_frame != nullptr) {
            const auto *header = BPlusTreeHeaderPage::Cast(header_frame);
            page_id_t root_pid = header->GetRootPageId();
            bpm_->UnpinPage(header_page_id_, false);
            return root_pid;
        }
    }
    return root_page_id_;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
void BPlusTree<KeyType, ValueType, KeyComparator>::SetRootPageId(page_id_t root_page_id) {
    root_page_id_ = root_page_id;
    if (header_page_id_ != INVALID_PAGE_ID && bpm_ != nullptr) {
        Page *header_frame = bpm_->FetchPage(header_page_id_);
        if (header_frame != nullptr) {
            auto *header = BPlusTreeHeaderPage::Cast(header_frame);
            header->SetRootPageId(root_page_id);
            bpm_->UnpinPage(header_page_id_, true);
        }
    }
}

template <typename KeyType, typename ValueType, typename KeyComparator>
bool BPlusTree<KeyType, ValueType, KeyComparator>::IsEmpty() const {
    return GetRootPageId() == INVALID_PAGE_ID;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
uint32_t BPlusTree<KeyType, ValueType, KeyComparator>::GetDepth() const {
    if (header_page_id_ != INVALID_PAGE_ID && bpm_ != nullptr) {
        Page *header_frame = bpm_->FetchPage(header_page_id_);
        if (header_frame != nullptr) {
            const auto *header = BPlusTreeHeaderPage::Cast(header_frame);
            uint32_t depth = header->GetDepth();
            bpm_->UnpinPage(header_page_id_, false);
            return depth;
        }
    }
    return (GetRootPageId() == INVALID_PAGE_ID) ? 0 : 1;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
uint64_t BPlusTree<KeyType, ValueType, KeyComparator>::GetRecordCount() const {
    if (header_page_id_ != INVALID_PAGE_ID && bpm_ != nullptr) {
        Page *header_frame = bpm_->FetchPage(header_page_id_);
        if (header_frame != nullptr) {
            const auto *header = BPlusTreeHeaderPage::Cast(header_frame);
            uint64_t count = header->GetRecordCount();
            bpm_->UnpinPage(header_page_id_, false);
            return count;
        }
    }
    return 0;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
Page *BPlusTree<KeyType, ValueType, KeyComparator>::FindLeftmostLeafPage() const {
    page_id_t curr_pid = GetRootPageId();
    if (curr_pid == INVALID_PAGE_ID || bpm_ == nullptr) {
        return nullptr;
    }

    Page *curr_page = bpm_->FetchPage(curr_pid);
    if (curr_page == nullptr) {
        return nullptr;
    }

    auto *tree_page = BPlusTreePage::Cast(curr_page);
    while (!tree_page->IsLeafPage()) {
        auto *internal_page = InternalPage::Cast(curr_page);
        page_id_t next_pid = internal_page->ValueAt(0);

        if (next_pid == INVALID_PAGE_ID) {
            bpm_->UnpinPage(curr_pid, false);
            return nullptr;
        }

        Page *next_page = bpm_->FetchPage(next_pid);
        bpm_->UnpinPage(curr_pid, false);

        if (next_page == nullptr) {
            return nullptr;
        }

        curr_page = next_page;
        curr_pid = next_pid;
        tree_page = BPlusTreePage::Cast(curr_page);
    }

    return curr_page;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
Page *BPlusTree<KeyType, ValueType, KeyComparator>::FindLeafPage(const KeyType &key, bool left_most) const {
    if (left_most) {
        return FindLeftmostLeafPage();
    }

    page_id_t curr_pid = GetRootPageId();
    if (curr_pid == INVALID_PAGE_ID || bpm_ == nullptr) {
        return nullptr;
    }

    Page *curr_page = bpm_->FetchPage(curr_pid);
    if (curr_page == nullptr) {
        return nullptr;
    }

    auto *tree_page = BPlusTreePage::Cast(curr_page);
    while (!tree_page->IsLeafPage()) {
        auto *internal_page = InternalPage::Cast(curr_page);
        page_id_t next_pid = internal_page->Lookup(key, comparator_);

        if (next_pid == INVALID_PAGE_ID) {
            bpm_->UnpinPage(curr_pid, false);
            return nullptr;
        }

        Page *next_page = bpm_->FetchPage(next_pid);
        bpm_->UnpinPage(curr_pid, false);

        if (next_page == nullptr) {
            return nullptr;
        }

        curr_page = next_page;
        curr_pid = next_pid;
        tree_page = BPlusTreePage::Cast(curr_page);
    }

    return curr_page;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
bool BPlusTree<KeyType, ValueType, KeyComparator>::GetValue(const KeyType &key, ValueType *result) const {
    std::shared_lock<std::shared_mutex> lock(*rwlock_);

    Page *leaf_page = FindLeafPage(key, false);
    if (leaf_page == nullptr) {
        return false;
    }

    auto *leaf = LeafPage::Cast(leaf_page);
    bool found = leaf->Lookup(key, result, comparator_);

    bpm_->UnpinPage(leaf_page->GetPageId(), false);
    return found;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
bool BPlusTree<KeyType, ValueType, KeyComparator>::GetValue(const KeyType &key, std::vector<ValueType> *result) const {
    ValueType val;
    if (GetValue(key, &val)) {
        if (result != nullptr) {
            result->clear();
            result->push_back(val);
        }
        return true;
    }
    return false;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
bool BPlusTree<KeyType, ValueType, KeyComparator>::Contains(const KeyType &key) const {
    ValueType val;
    return GetValue(key, &val);
}

template <typename KeyType, typename ValueType, typename KeyComparator>
typename BPlusTree<KeyType, ValueType, KeyComparator>::INDEXITERATOR_TYPE
BPlusTree<KeyType, ValueType, KeyComparator>::Begin() const {
    std::shared_lock<std::shared_mutex> lock(*rwlock_);
    if (IsEmpty()) {
        return End();
    }

    Page *leaf_page = FindLeftmostLeafPage();
    if (leaf_page == nullptr) {
        return End();
    }

    auto *leaf = LeafPage::Cast(leaf_page);
    if (leaf->GetSize() == 0) {
        bpm_->UnpinPage(leaf_page->GetPageId(), false);
        return End();
    }

    return INDEXITERATOR_TYPE(bpm_, leaf_page, 0);
}

template <typename KeyType, typename ValueType, typename KeyComparator>
typename BPlusTree<KeyType, ValueType, KeyComparator>::INDEXITERATOR_TYPE
BPlusTree<KeyType, ValueType, KeyComparator>::Begin(const KeyType &key) const {
    std::shared_lock<std::shared_mutex> lock(*rwlock_);
    if (IsEmpty()) {
        return End();
    }

    Page *curr_page = FindLeafPage(key, false);
    if (curr_page == nullptr) {
        return End();
    }

    while (curr_page != nullptr) {
        auto *leaf = LeafPage::Cast(curr_page);
        int idx = leaf->LowerBound(key, comparator_);
        if (idx < leaf->GetSize()) {
            return INDEXITERATOR_TYPE(bpm_, curr_page, idx);
        }
        page_id_t next_pid = leaf->GetNextPageId();
        bpm_->UnpinPage(curr_page->GetPageId(), false);
        if (next_pid == INVALID_PAGE_ID) {
            return End();
        }
        curr_page = bpm_->FetchPage(next_pid);
    }

    return End();
}

template <typename KeyType, typename ValueType, typename KeyComparator>
size_t BPlusTree<KeyType, ValueType, KeyComparator>::RangeScan(const KeyType &low_key, const KeyType &high_key,
                                                              std::vector<std::pair<KeyType, ValueType>> *result) const {
    if (result != nullptr) {
        result->clear();
    }
    if (Compare(comparator_, low_key, high_key) > 0) {
        return 0;
    }
    size_t count = 0;
    for (auto it = Begin(low_key); it != End() && Compare(comparator_, it->first, high_key) <= 0; ++it) {
        if (result != nullptr) {
            result->push_back(*it);
        }
        count++;
    }
    return count;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
size_t BPlusTree<KeyType, ValueType, KeyComparator>::RangeScan(const KeyType &low_key, const KeyType &high_key,
                                                              std::vector<ValueType> *result) const {
    if (result != nullptr) {
        result->clear();
    }
    if (Compare(comparator_, low_key, high_key) > 0) {
        return 0;
    }
    size_t count = 0;
    for (auto it = Begin(low_key); it != End() && Compare(comparator_, it->first, high_key) <= 0; ++it) {
        if (result != nullptr) {
            result->push_back(it->second);
        }
        count++;
    }
    return count;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
bool BPlusTree<KeyType, ValueType, KeyComparator>::Insert(const KeyType &key, const ValueType &value) {
    std::unique_lock<std::shared_mutex> lock(*rwlock_);

    if (IsEmpty()) {
        return InsertIntoEmptyTreeInternal(key, value);
    }

    return InsertIntoLeafInternal(key, value);
}

template <typename KeyType, typename ValueType, typename KeyComparator>
bool BPlusTree<KeyType, ValueType, KeyComparator>::InsertIntoEmptyTree(const KeyType &key, const ValueType &value) {
    std::unique_lock<std::shared_mutex> lock(*rwlock_);
    return InsertIntoEmptyTreeInternal(key, value);
}

template <typename KeyType, typename ValueType, typename KeyComparator>
bool BPlusTree<KeyType, ValueType, KeyComparator>::StartNewTree(const KeyType &key, const ValueType &value) {
    return InsertIntoEmptyTree(key, value);
}

template <typename KeyType, typename ValueType, typename KeyComparator>
bool BPlusTree<KeyType, ValueType, KeyComparator>::InsertIntoLeaf(const KeyType &key, const ValueType &value) {
    std::unique_lock<std::shared_mutex> lock(*rwlock_);
    return InsertIntoLeafInternal(key, value);
}

template <typename KeyType, typename ValueType, typename KeyComparator>
bool BPlusTree<KeyType, ValueType, KeyComparator>::InsertIntoEmptyTreeInternal(const KeyType &key, const ValueType &value) {
    if (!IsEmpty()) {
        return false;
    }

    page_id_t root_pid = INVALID_PAGE_ID;
    Page *root_frame = bpm_->NewPage(&root_pid);
    if (root_frame == nullptr) {
        return false;
    }

    auto *root = LeafPage::Cast(root_frame);
    root->Init(root_pid, INVALID_PAGE_ID, leaf_max_size_);
    root->Insert(key, value, comparator_);

    SetRootPageId(root_pid);
    UpdateHeaderMetadata(root_pid, 1, 1);

    bpm_->UnpinPage(root_pid, true);
    return true;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
bool BPlusTree<KeyType, ValueType, KeyComparator>::InsertIntoLeafInternal(const KeyType &key, const ValueType &value) {
    if (IsEmpty()) {
        return InsertIntoEmptyTreeInternal(key, value);
    }

    Page *leaf_frame = FindLeafPage(key, false);
    if (leaf_frame == nullptr) {
        return false;
    }

    auto *leaf = LeafPage::Cast(leaf_frame);

    if (leaf->KeyIndex(key, comparator_) != -1) {
        bpm_->UnpinPage(leaf_frame->GetPageId(), false);
        return false;
    }

    if (leaf->GetSize() < leaf->GetMaxSize()) {
        leaf->Insert(key, value, comparator_);
        UpdateRecordCount(1);
        bpm_->UnpinPage(leaf_frame->GetPageId(), true);
        return true;
    }

    LeafPage *new_leaf = Split(leaf);
    if (new_leaf == nullptr) {
        bpm_->UnpinPage(leaf_frame->GetPageId(), false);
        return false;
    }

    if (Compare(comparator_, key, new_leaf->KeyAt(0)) < 0) {
        leaf->Insert(key, value, comparator_);
    } else {
        new_leaf->Insert(key, value, comparator_);
    }

    KeyType separator_key = new_leaf->KeyAt(0);
    InsertIntoParent(leaf, separator_key, new_leaf);
    UpdateRecordCount(1);

    bpm_->UnpinPage(new_leaf->GetPageId(), true);
    bpm_->UnpinPage(leaf_frame->GetPageId(), true);
    return true;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
page_id_t BPlusTree<KeyType, ValueType, KeyComparator>::CreateNewRoot(BPlusTreePage *old_root,
                                                                      const KeyType &separator_key,
                                                                      BPlusTreePage *new_sibling) {
    if (old_root == nullptr || new_sibling == nullptr || bpm_ == nullptr) {
        return INVALID_PAGE_ID;
    }

    page_id_t new_root_pid = INVALID_PAGE_ID;
    Page *new_root_frame = bpm_->NewPage(&new_root_pid);
    if (new_root_frame == nullptr) {
        return INVALID_PAGE_ID;
    }

    auto *new_root = InternalPage::Cast(new_root_frame);
    new_root->Init(new_root_pid, INVALID_PAGE_ID, internal_max_size_);
    new_root->Populate(old_root->GetPageId(), separator_key, new_sibling->GetPageId());

    old_root->SetParentPageId(new_root_pid);
    new_sibling->SetParentPageId(new_root_pid);

    uint32_t new_depth = GetDepth() + 1;
    SetRootPageId(new_root_pid);
    UpdateHeaderMetadata(new_root_pid, new_depth, 0);

    bpm_->UnpinPage(new_root_pid, true);
    return new_root_pid;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
typename BPlusTree<KeyType, ValueType, KeyComparator>::LeafPage *
BPlusTree<KeyType, ValueType, KeyComparator>::Split(LeafPage *leaf) {
    if (leaf == nullptr || bpm_ == nullptr) {
        return nullptr;
    }

    page_id_t new_leaf_pid = INVALID_PAGE_ID;
    Page *new_leaf_frame = bpm_->NewPage(&new_leaf_pid);
    if (new_leaf_frame == nullptr) {
        return nullptr;
    }

    auto *new_leaf = LeafPage::Cast(new_leaf_frame);
    new_leaf->Init(new_leaf_pid, leaf->GetParentPageId(), leaf_max_size_);

    leaf->MoveHalfTo(new_leaf);
    return new_leaf;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
typename BPlusTree<KeyType, ValueType, KeyComparator>::InternalPage *
BPlusTree<KeyType, ValueType, KeyComparator>::Split(InternalPage *internal) {
    if (internal == nullptr || bpm_ == nullptr) {
        return nullptr;
    }

    page_id_t new_internal_pid = INVALID_PAGE_ID;
    Page *new_internal_frame = bpm_->NewPage(&new_internal_pid);
    if (new_internal_frame == nullptr) {
        return nullptr;
    }

    auto *new_internal = InternalPage::Cast(new_internal_frame);
    new_internal->Init(new_internal_pid, internal->GetParentPageId(), internal_max_size_);

    internal->MoveHalfTo(new_internal, bpm_);
    return new_internal;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
typename BPlusTree<KeyType, ValueType, KeyComparator>::InternalPage *
BPlusTree<KeyType, ValueType, KeyComparator>::SplitInternal(InternalPage *parent, page_id_t old_child_pid,
                                                           const KeyType &separator_key, page_id_t new_child_pid,
                                                           KeyType *split_up_key) {
    if (parent == nullptr || bpm_ == nullptr || split_up_key == nullptr) {
        return nullptr;
    }

    page_id_t new_parent_pid = INVALID_PAGE_ID;
    Page *new_parent_frame = bpm_->NewPage(&new_parent_pid);
    if (new_parent_frame == nullptr) {
        return nullptr;
    }

    auto *new_parent = InternalPage::Cast(new_parent_frame);
    new_parent->Init(new_parent_pid, parent->GetParentPageId(), internal_max_size_);

    std::vector<std::pair<KeyType, page_id_t>> entries;
    int old_idx = parent->ValueIndex(old_child_pid);
    for (int i = 0; i <= old_idx; ++i) {
        entries.push_back({parent->KeyAt(i), parent->ValueAt(i)});
    }
    entries.push_back({separator_key, new_child_pid});
    for (int i = old_idx + 1; i < parent->GetSize(); ++i) {
        entries.push_back({parent->KeyAt(i), parent->ValueAt(i)});
    }

    int total_entries = static_cast<int>(entries.size());
    int mid = total_entries / 2;
    *split_up_key = entries[mid].first;

    parent->SetSize(mid);
    for (int i = 0; i < mid; ++i) {
        parent->SetKeyAt(i, entries[i].first);
        parent->SetValueAt(i, entries[i].second);
    }

    int new_size = total_entries - mid;
    new_parent->SetSize(new_size);
    new_parent->SetValueAt(0, entries[mid].second);
    for (int i = 1; i < new_size; ++i) {
        new_parent->SetKeyAt(i, entries[mid + i].first);
        new_parent->SetValueAt(i, entries[mid + i].second);
    }

    for (int i = 0; i < new_size; ++i) {
        page_id_t child_pid = new_parent->ValueAt(i);
        Page *child_frame = bpm_->FetchPage(child_pid);
        if (child_frame != nullptr) {
            BPlusTreePage::Cast(child_frame)->SetParentPageId(new_parent_pid);
            bpm_->UnpinPage(child_pid, true);
        }
    }

    if (parent->ValueIndex(new_child_pid) != -1) {
        Page *child_frame = bpm_->FetchPage(new_child_pid);
        if (child_frame != nullptr) {
            BPlusTreePage::Cast(child_frame)->SetParentPageId(parent->GetPageId());
            bpm_->UnpinPage(new_child_pid, true);
        }
    }

    return new_parent;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
void BPlusTree<KeyType, ValueType, KeyComparator>::InsertIntoParent(BPlusTreePage *old_child,
                                                                   const KeyType &separator_key,
                                                                   BPlusTreePage *new_child) {
    page_id_t parent_pid = old_child->GetParentPageId();

    if (parent_pid == INVALID_PAGE_ID) {
        CreateNewRoot(old_child, separator_key, new_child);
        return;
    }

    Page *parent_frame = bpm_->FetchPage(parent_pid);
    if (parent_frame == nullptr) return;

    auto *parent = InternalPage::Cast(parent_frame);

    if (parent->GetSize() < parent->GetMaxSize()) {
        parent->InsertNodeAfter(old_child->GetPageId(), separator_key, new_child->GetPageId());
        new_child->SetParentPageId(parent_pid);
        bpm_->UnpinPage(parent_pid, true);
        return;
    }

    KeyType split_up_key{};
    InternalPage *new_parent = SplitInternal(parent, old_child->GetPageId(), separator_key,
                                             new_child->GetPageId(), &split_up_key);
    if (new_parent == nullptr) {
        bpm_->UnpinPage(parent_pid, false);
        return;
    }

    page_id_t new_parent_pid = new_parent->GetPageId();
    InsertIntoParent(parent, split_up_key, new_parent);

    bpm_->UnpinPage(new_parent_pid, true);
    bpm_->UnpinPage(parent_pid, true);
}

template <typename KeyType, typename ValueType, typename KeyComparator>
void BPlusTree<KeyType, ValueType, KeyComparator>::UpdateHeaderMetadata(page_id_t root_pid, uint32_t depth,
                                                                        int64_t record_count_delta) {
    if (header_page_id_ != INVALID_PAGE_ID && bpm_ != nullptr) {
        Page *header_frame = bpm_->FetchPage(header_page_id_);
        if (header_frame != nullptr) {
            auto *header = BPlusTreeHeaderPage::Cast(header_frame);
            if (root_pid != INVALID_PAGE_ID) {
                header->SetRootPageId(root_pid);
            }
            if (depth > 0) {
                header->SetDepth(depth);
            }
            if (record_count_delta != 0) {
                header->IncreaseRecordCount(record_count_delta);
            }
            bpm_->UnpinPage(header_page_id_, true);
        }
    }
}

template <typename KeyType, typename ValueType, typename KeyComparator>
void BPlusTree<KeyType, ValueType, KeyComparator>::UpdateRecordCount(int64_t delta) {
    if (header_page_id_ != INVALID_PAGE_ID && bpm_ != nullptr) {
        Page *header_frame = bpm_->FetchPage(header_page_id_);
        if (header_frame != nullptr) {
            auto *header = BPlusTreeHeaderPage::Cast(header_frame);
            header->IncreaseRecordCount(delta);
            bpm_->UnpinPage(header_page_id_, true);
        }
    }
}

template <typename KeyType, typename ValueType, typename KeyComparator>
bool BPlusTree<KeyType, ValueType, KeyComparator>::Remove(const KeyType &key) {
    std::unique_lock<std::shared_mutex> lock(*rwlock_);

    if (IsEmpty()) {
        return false;
    }

    Page *leaf_frame = FindLeafPage(key, false);
    if (leaf_frame == nullptr) {
        return false;
    }

    auto *leaf = LeafPage::Cast(leaf_frame);
    int key_idx = leaf->KeyIndex(key, comparator_);
    if (key_idx == -1) {
        bpm_->UnpinPage(leaf_frame->GetPageId(), false);
        return false;
    }

    leaf->RemoveAndDeleteRecord(key, comparator_);
    UpdateRecordCount(-1);

    if (leaf->IsRootPage()) {
        if (leaf->GetSize() == 0) {
            AdjustRoot(leaf);
            return true;
        }
        bpm_->UnpinPage(leaf->GetPageId(), true);
        return true;
    }

    int min_size = leaf->GetMinSize();
    if (leaf->GetSize() < min_size) {
        bool leaf_deleted = false;
        CoalesceOrRedistribute(leaf, &leaf_deleted);
        if (!leaf_deleted) {
            bpm_->UnpinPage(leaf->GetPageId(), true);
        }
        return true;
    }

    bpm_->UnpinPage(leaf->GetPageId(), true);
    return true;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
bool BPlusTree<KeyType, ValueType, KeyComparator>::Delete(const KeyType &key) {
    return Remove(key);
}

template <typename KeyType, typename ValueType, typename KeyComparator>
bool BPlusTree<KeyType, ValueType, KeyComparator>::RedistributeBorrowFromLeft(LeafPage *leaf,
                                                                             LeafPage *left_sibling,
                                                                             InternalPage *parent,
                                                                             int parent_idx) {
    if (leaf == nullptr || left_sibling == nullptr || parent == nullptr || parent_idx <= 0) {
        return false;
    }
    if (left_sibling->GetSize() <= left_sibling->GetMinSize()) {
        return false;
    }

    left_sibling->MoveLastToFrontOf(leaf);
    parent->SetKeyAt(parent_idx, leaf->KeyAt(0));
    return true;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
bool BPlusTree<KeyType, ValueType, KeyComparator>::RedistributeBorrowFromRight(LeafPage *leaf,
                                                                              LeafPage *right_sibling,
                                                                              InternalPage *parent,
                                                                              int parent_idx) {
    if (leaf == nullptr || right_sibling == nullptr || parent == nullptr || parent_idx < 0 ||
        parent_idx >= parent->GetSize() - 1) {
        return false;
    }
    if (right_sibling->GetSize() <= right_sibling->GetMinSize()) {
        return false;
    }

    right_sibling->MoveFirstToEndOf(leaf);
    parent->SetKeyAt(parent_idx + 1, right_sibling->KeyAt(0));
    if (parent_idx > 0) {
        parent->SetKeyAt(parent_idx, leaf->KeyAt(0));
    }
    return true;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
bool BPlusTree<KeyType, ValueType, KeyComparator>::Redistribute(LeafPage *leaf) {
    if (leaf == nullptr || leaf->IsRootPage() || bpm_ == nullptr) {
        return false;
    }
    if (leaf->GetSize() >= leaf->GetMinSize()) {
        return false;
    }

    page_id_t parent_pid = leaf->GetParentPageId();
    if (parent_pid == INVALID_PAGE_ID) {
        return false;
    }

    Page *parent_frame = bpm_->FetchPage(parent_pid);
    if (parent_frame == nullptr) {
        return false;
    }

    auto *parent = InternalPage::Cast(parent_frame);
    int parent_idx = parent->ValueIndex(leaf->GetPageId());
    if (parent_idx == -1) {
        bpm_->UnpinPage(parent_pid, false);
        return false;
    }

    if (parent_idx > 0) {
        page_id_t left_pid = parent->ValueAt(parent_idx - 1);
        Page *left_frame = bpm_->FetchPage(left_pid);
        if (left_frame != nullptr) {
            auto *left_sibling = LeafPage::Cast(left_frame);
            if (RedistributeBorrowFromLeft(leaf, left_sibling, parent, parent_idx)) {
                bpm_->UnpinPage(left_pid, true);
                bpm_->UnpinPage(parent_pid, true);
                return true;
            }
            bpm_->UnpinPage(left_pid, false);
        }
    }

    if (parent_idx < parent->GetSize() - 1) {
        page_id_t right_pid = parent->ValueAt(parent_idx + 1);
        Page *right_frame = bpm_->FetchPage(right_pid);
        if (right_frame != nullptr) {
            auto *right_sibling = LeafPage::Cast(right_frame);
            if (RedistributeBorrowFromRight(leaf, right_sibling, parent, parent_idx)) {
                bpm_->UnpinPage(right_pid, true);
                bpm_->UnpinPage(parent_pid, true);
                return true;
            }
            bpm_->UnpinPage(right_pid, false);
        }
    }

    bpm_->UnpinPage(parent_pid, false);
    return false;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
bool BPlusTree<KeyType, ValueType, KeyComparator>::Coalesce(LeafPage *leaf, bool *leaf_deleted) {
    bool dummy = false;
    if (leaf_deleted == nullptr) {
        leaf_deleted = &dummy;
    }
    *leaf_deleted = false;

    if (leaf == nullptr || leaf->IsRootPage() || bpm_ == nullptr) {
        return false;
    }

    page_id_t parent_pid = leaf->GetParentPageId();
    if (parent_pid == INVALID_PAGE_ID) {
        return false;
    }

    Page *parent_frame = bpm_->FetchPage(parent_pid);
    if (parent_frame == nullptr) {
        return false;
    }

    auto *parent = InternalPage::Cast(parent_frame);
    int parent_idx = parent->ValueIndex(leaf->GetPageId());
    if (parent_idx == -1) {
        bpm_->UnpinPage(parent_pid, false);
        return false;
    }

    if (parent_idx > 0) {
        page_id_t left_pid = parent->ValueAt(parent_idx - 1);
        Page *left_frame = bpm_->FetchPage(left_pid);
        if (left_frame != nullptr) {
            auto *left_sibling = LeafPage::Cast(left_frame);
            if (left_sibling->GetSize() + leaf->GetSize() <= leaf_max_size_) {
                leaf->MoveAllTo(left_sibling);
                parent->Remove(parent_idx);
                bpm_->UnpinPage(left_pid, true);

                page_id_t leaf_pid = leaf->GetPageId();
                bpm_->UnpinPage(leaf_pid, false);
                bpm_->DeletePage(leaf_pid);
                *leaf_deleted = true;

                HandleParentUnderflow(parent);
                return true;
            }
            bpm_->UnpinPage(left_pid, false);
        }
    }

    if (parent_idx < parent->GetSize() - 1) {
        page_id_t right_pid = parent->ValueAt(parent_idx + 1);
        Page *right_frame = bpm_->FetchPage(right_pid);
        if (right_frame != nullptr) {
            auto *right_sibling = LeafPage::Cast(right_frame);
            if (leaf->GetSize() + right_sibling->GetSize() <= leaf_max_size_) {
                right_sibling->MoveAllTo(leaf);
                parent->Remove(parent_idx + 1);

                bpm_->UnpinPage(right_pid, false);
                bpm_->DeletePage(right_pid);
                *leaf_deleted = false;

                HandleParentUnderflow(parent);
                return true;
            }
            bpm_->UnpinPage(right_pid, false);
        }
    }

    bpm_->UnpinPage(parent_pid, false);
    return false;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
bool BPlusTree<KeyType, ValueType, KeyComparator>::Redistribute(InternalPage *internal) {
    if (internal == nullptr || internal->IsRootPage() || bpm_ == nullptr) {
        return false;
    }
    if (internal->GetSize() >= internal->GetMinSize()) {
        return false;
    }

    page_id_t parent_pid = internal->GetParentPageId();
    if (parent_pid == INVALID_PAGE_ID) {
        return false;
    }

    Page *parent_frame = bpm_->FetchPage(parent_pid);
    if (parent_frame == nullptr) {
        return false;
    }

    auto *parent = InternalPage::Cast(parent_frame);
    int parent_idx = parent->ValueIndex(internal->GetPageId());
    if (parent_idx == -1) {
        bpm_->UnpinPage(parent_pid, false);
        return false;
    }

    if (parent_idx > 0) {
        page_id_t left_pid = parent->ValueAt(parent_idx - 1);
        Page *left_frame = bpm_->FetchPage(left_pid);
        if (left_frame != nullptr) {
            auto *left_sibling = InternalPage::Cast(left_frame);
            if (left_sibling->GetSize() > left_sibling->GetMinSize()) {
                KeyType middle_key = parent->KeyAt(parent_idx);
                KeyType pushed_up_key = left_sibling->KeyAt(left_sibling->GetSize() - 1);

                left_sibling->MoveLastToFrontOf(internal, middle_key, bpm_);
                parent->SetKeyAt(parent_idx, pushed_up_key);

                bpm_->UnpinPage(left_pid, true);
                bpm_->UnpinPage(parent_pid, true);
                return true;
            }
            bpm_->UnpinPage(left_pid, false);
        }
    }

    if (parent_idx < parent->GetSize() - 1) {
        page_id_t right_pid = parent->ValueAt(parent_idx + 1);
        Page *right_frame = bpm_->FetchPage(right_pid);
        if (right_frame != nullptr) {
            auto *right_sibling = InternalPage::Cast(right_frame);
            if (right_sibling->GetSize() > right_sibling->GetMinSize()) {
                KeyType middle_key = parent->KeyAt(parent_idx + 1);
                KeyType pushed_up_key = right_sibling->KeyAt(1);

                right_sibling->MoveFirstToEndOf(internal, middle_key, bpm_);
                parent->SetKeyAt(parent_idx + 1, pushed_up_key);

                bpm_->UnpinPage(right_pid, true);
                bpm_->UnpinPage(parent_pid, true);
                return true;
            }
            bpm_->UnpinPage(right_pid, false);
        }
    }

    bpm_->UnpinPage(parent_pid, false);
    return false;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
bool BPlusTree<KeyType, ValueType, KeyComparator>::Coalesce(InternalPage *internal, bool *internal_deleted) {
    bool dummy = false;
    if (internal_deleted == nullptr) {
        internal_deleted = &dummy;
    }
    *internal_deleted = false;

    if (internal == nullptr || internal->IsRootPage() || bpm_ == nullptr) {
        return false;
    }

    page_id_t parent_pid = internal->GetParentPageId();
    if (parent_pid == INVALID_PAGE_ID) {
        return false;
    }

    Page *parent_frame = bpm_->FetchPage(parent_pid);
    if (parent_frame == nullptr) {
        return false;
    }

    auto *parent = InternalPage::Cast(parent_frame);
    int parent_idx = parent->ValueIndex(internal->GetPageId());
    if (parent_idx == -1) {
        bpm_->UnpinPage(parent_pid, false);
        return false;
    }

    if (parent_idx > 0) {
        page_id_t left_pid = parent->ValueAt(parent_idx - 1);
        Page *left_frame = bpm_->FetchPage(left_pid);
        if (left_frame != nullptr) {
            auto *left_sibling = InternalPage::Cast(left_frame);
            if (left_sibling->GetSize() + internal->GetSize() <= internal_max_size_) {
                KeyType middle_key = parent->KeyAt(parent_idx);

                internal->MoveAllTo(left_sibling, middle_key, bpm_);
                parent->Remove(parent_idx);
                bpm_->UnpinPage(left_pid, true);

                page_id_t internal_pid = internal->GetPageId();
                bpm_->UnpinPage(internal_pid, false);
                bpm_->DeletePage(internal_pid);
                *internal_deleted = true;

                HandleParentUnderflow(parent);
                return true;
            }
            bpm_->UnpinPage(left_pid, false);
        }
    }

    if (parent_idx < parent->GetSize() - 1) {
        page_id_t right_pid = parent->ValueAt(parent_idx + 1);
        Page *right_frame = bpm_->FetchPage(right_pid);
        if (right_frame != nullptr) {
            auto *right_sibling = InternalPage::Cast(right_frame);
            if (internal->GetSize() + right_sibling->GetSize() <= internal_max_size_) {
                KeyType middle_key = parent->KeyAt(parent_idx + 1);

                right_sibling->MoveAllTo(internal, middle_key, bpm_);
                parent->Remove(parent_idx + 1);

                bpm_->UnpinPage(right_pid, false);
                bpm_->DeletePage(right_pid);
                *internal_deleted = false;

                HandleParentUnderflow(parent);
                return true;
            }
            bpm_->UnpinPage(right_pid, false);
        }
    }

    bpm_->UnpinPage(parent_pid, false);
    return false;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
void BPlusTree<KeyType, ValueType, KeyComparator>::HandleParentUnderflow(InternalPage *parent) {
    if (parent == nullptr || bpm_ == nullptr) return;

    if (parent->IsRootPage() || parent->GetPageId() == GetRootPageId()) {
        if (parent->GetSize() <= 1) {
            AdjustRoot(parent);
        } else {
            bpm_->UnpinPage(parent->GetPageId(), true);
        }
        return;
    }

    if (parent->GetSize() < parent->GetMinSize()) {
        bool parent_deleted = false;
        CoalesceOrRedistribute(parent, &parent_deleted);
        if (!parent_deleted) {
            bpm_->UnpinPage(parent->GetPageId(), true);
        }
    } else {
        bpm_->UnpinPage(parent->GetPageId(), true);
    }
}

template <typename KeyType, typename ValueType, typename KeyComparator>
bool BPlusTree<KeyType, ValueType, KeyComparator>::AdjustRoot(BPlusTreePage *old_root) {
    if (old_root == nullptr || (!old_root->IsRootPage() && old_root->GetPageId() != GetRootPageId()) || bpm_ == nullptr) {
        return false;
    }

    if (old_root->IsLeafPage()) {
        if (old_root->GetSize() == 0) {
            page_id_t old_pid = old_root->GetPageId();
            bpm_->UnpinPage(old_pid, false);
            bpm_->DeletePage(old_pid);
            SetRootPageId(INVALID_PAGE_ID);
            return true;
        }
        return false;
    }

    auto *internal_root = static_cast<InternalPage *>(old_root);
    if (internal_root->GetSize() <= 1) {
        if (internal_root->GetSize() == 0) {
            page_id_t old_root_pid = internal_root->GetPageId();
            bpm_->UnpinPage(old_root_pid, false);
            bpm_->DeletePage(old_root_pid);
            SetRootPageId(INVALID_PAGE_ID);
            return true;
        }

        page_id_t new_root_pid = internal_root->ValueAt(0);
        page_id_t old_root_pid = internal_root->GetPageId();
        bpm_->UnpinPage(old_root_pid, false);
        bpm_->DeletePage(old_root_pid);

        uint32_t current_depth = GetDepth();
        uint32_t new_depth = (current_depth > 1) ? current_depth - 1 : 1;
        SetRootPageId(new_root_pid);
        UpdateHeaderMetadata(new_root_pid, new_depth, 0);

        Page *new_root_frame = bpm_->FetchPage(new_root_pid);
        if (new_root_frame != nullptr) {
            auto *new_root = BPlusTreePage::Cast(new_root_frame);
            new_root->SetParentPageId(INVALID_PAGE_ID);

            if (new_root->IsLeafPage()) {
                if (new_root->GetSize() == 0) {
                    bpm_->UnpinPage(new_root_pid, false);
                    bpm_->DeletePage(new_root_pid);
                    SetRootPageId(INVALID_PAGE_ID);
                    return true;
                }
                bpm_->UnpinPage(new_root_pid, true);
                return true;
            }

            auto *promoted_internal = InternalPage::Cast(new_root_frame);
            if (promoted_internal->GetSize() <= 1) {
                return AdjustRoot(promoted_internal);
            }
            bpm_->UnpinPage(new_root_pid, true);
        }
        return true;
    }

    return false;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
bool BPlusTree<KeyType, ValueType, KeyComparator>::CoalesceOrRedistribute(InternalPage *internal, bool *internal_deleted) {
    bool dummy = false;
    if (internal_deleted == nullptr) {
        internal_deleted = &dummy;
    }
    *internal_deleted = false;

    if (Redistribute(internal)) {
        return true;
    }

    return Coalesce(internal, internal_deleted);
}

template <typename KeyType, typename ValueType, typename KeyComparator>
bool BPlusTree<KeyType, ValueType, KeyComparator>::CoalesceOrRedistribute(LeafPage *leaf, bool *leaf_deleted) {
    bool dummy = false;
    if (leaf_deleted == nullptr) {
        leaf_deleted = &dummy;
    }
    *leaf_deleted = false;

    if (Redistribute(leaf)) {
        return true;
    }

    return Coalesce(leaf, leaf_deleted);
}

template <typename KeyType, typename ValueType, typename KeyComparator>
TreeValidationResult BPlusTree<KeyType, ValueType, KeyComparator>::ValidateTree() const {
    std::shared_lock<std::shared_mutex> lock(*rwlock_);

    page_id_t root_pid = GetRootPageId();
    if (root_pid == INVALID_PAGE_ID) {
        if (GetRecordCount() != 0) {
            return {false, "Empty tree has non-zero record count: " + std::to_string(GetRecordCount())};
        }
        if (GetDepth() != 0) {
            return {false, "Empty tree has non-zero depth: " + std::to_string(GetDepth())};
        }
        return {true, "", 0, 0, 0, 0};
    }

    Page *root_frame = bpm_->FetchPage(root_pid);
    if (root_frame == nullptr) {
        return {false, "Failed to fetch root page frame " + std::to_string(root_pid)};
    }

    auto *root_page = BPlusTreePage::Cast(root_frame);
    if (root_page->GetParentPageId() != INVALID_PAGE_ID) {
        bpm_->UnpinPage(root_pid, false);
        return {false, "Root page has invalid parent_page_id: " + std::to_string(root_page->GetParentPageId())};
    }

    if (root_page->IsLeafPage()) {
        if (root_page->GetSize() < 1 || root_page->GetSize() > leaf_max_size_) {
            bpm_->UnpinPage(root_pid, false);
            return {false, "Root leaf page size out of bounds: " + std::to_string(root_page->GetSize())};
        }
    } else if (root_page->IsInternalPage()) {
        if (root_page->GetSize() < 2 || root_page->GetSize() > internal_max_size_) {
            bpm_->UnpinPage(root_pid, false);
            return {false, "Root internal page size out of bounds: " + std::to_string(root_page->GetSize())};
        }
    } else {
        bpm_->UnpinPage(root_pid, false);
        return {false, "Root page has invalid page type"};
    }
    bpm_->UnpinPage(root_pid, false);

    struct BFSItem {
        page_id_t page_id;
        uint32_t depth;
        std::optional<KeyType> lower_bound;
        std::optional<KeyType> upper_bound;
    };

    std::queue<BFSItem> q;
    std::unordered_set<page_id_t> visited_pages;
    std::set<page_id_t> hierarchy_leaves;
    size_t hierarchy_key_count = 0;
    size_t leaf_page_count = 0;
    size_t internal_page_count = 0;
    uint32_t observed_leaf_depth = 0;

    q.push({root_pid, 1, std::nullopt, std::nullopt});
    visited_pages.insert(root_pid);

    while (!q.empty()) {
        BFSItem item = q.front();
        q.pop();

        Page *frame = bpm_->FetchPage(item.page_id);
        if (frame == nullptr) {
            return {false, "Failed to fetch page " + std::to_string(item.page_id) + " during validation"};
        }

        auto *page = BPlusTreePage::Cast(frame);

        if (page->IsLeafPage()) {
            auto *leaf = LeafPage::Cast(frame);
            leaf_page_count++;

            if (observed_leaf_depth == 0) {
                observed_leaf_depth = item.depth;
            } else if (observed_leaf_depth != item.depth) {
                bpm_->UnpinPage(item.page_id, false);
                return {false, "Leaf page " + std::to_string(item.page_id) + " at depth "
                               + std::to_string(item.depth) + " but expected depth " + std::to_string(observed_leaf_depth)};
            }

            if (item.page_id != root_pid) {
                int min_leaf = leaf_max_size_ / 2;
                if (leaf->GetSize() < min_leaf || leaf->GetSize() > leaf_max_size_) {
                    bpm_->UnpinPage(item.page_id, false);
                    return {false, "Leaf page " + std::to_string(item.page_id) + " occupancy violation: size "
                                   + std::to_string(leaf->GetSize()) + " not in [" + std::to_string(min_leaf) + ", "
                                   + std::to_string(leaf_max_size_) + "]"};
                }
            }

            for (int i = 0; i < leaf->GetSize(); ++i) {
                const KeyType &k = leaf->KeyAt(i);
                if (item.lower_bound.has_value() && Compare(comparator_, k, *item.lower_bound) < 0) {
                    bpm_->UnpinPage(item.page_id, false);
                    return {false, "Key in leaf " + std::to_string(item.page_id) + " violates lower bound"};
                }
                if (item.upper_bound.has_value() && Compare(comparator_, k, *item.upper_bound) >= 0) {
                    bpm_->UnpinPage(item.page_id, false);
                    return {false, "Key in leaf " + std::to_string(item.page_id) + " violates upper bound"};
                }
                if (i > 0 && Compare(comparator_, leaf->KeyAt(i - 1), k) >= 0) {
                    bpm_->UnpinPage(item.page_id, false);
                    return {false, "Leaf " + std::to_string(item.page_id) + " keys not strictly ascending"};
                }
            }

            hierarchy_leaves.insert(item.page_id);
            hierarchy_key_count += leaf->GetSize();
            bpm_->UnpinPage(item.page_id, false);

        } else if (page->IsInternalPage()) {
            auto *internal = InternalPage::Cast(frame);
            internal_page_count++;

            if (item.page_id != root_pid) {
                int min_internal = (internal_max_size_ + 1) / 2;
                if (internal->GetSize() < min_internal || internal->GetSize() > internal_max_size_) {
                    bpm_->UnpinPage(item.page_id, false);
                    return {false, "Internal page " + std::to_string(item.page_id) + " occupancy violation: size "
                                   + std::to_string(internal->GetSize()) + " not in [" + std::to_string(min_internal) + ", "
                                   + std::to_string(internal_max_size_) + "]"};
                }
            }

            for (int i = 1; i < internal->GetSize(); ++i) {
                const KeyType &k = internal->KeyAt(i);
                if (item.lower_bound.has_value() && Compare(comparator_, k, *item.lower_bound) < 0) {
                    bpm_->UnpinPage(item.page_id, false);
                    return {false, "Separator key in internal page " + std::to_string(item.page_id) + " violates lower bound"};
                }
                if (item.upper_bound.has_value() && Compare(comparator_, k, *item.upper_bound) >= 0) {
                    bpm_->UnpinPage(item.page_id, false);
                    return {false, "Separator key in internal page " + std::to_string(item.page_id) + " violates upper bound"};
                }
                if (i > 1 && Compare(comparator_, internal->KeyAt(i - 1), k) >= 0) {
                    bpm_->UnpinPage(item.page_id, false);
                    return {false, "Internal page " + std::to_string(item.page_id) + " keys not strictly ascending"};
                }
            }

            for (int i = 0; i < internal->GetSize(); ++i) {
                page_id_t child_pid = internal->ValueAt(i);
                if (child_pid == INVALID_PAGE_ID) {
                    bpm_->UnpinPage(item.page_id, false);
                    return {false, "Internal page " + std::to_string(item.page_id) + " has child pointer INVALID_PAGE_ID"};
                }
                if (visited_pages.count(child_pid)) {
                    bpm_->UnpinPage(item.page_id, false);
                    return {false, "Cycle detected in tree: page " + std::to_string(child_pid) + " visited twice"};
                }
                visited_pages.insert(child_pid);

                Page *child_frame = bpm_->FetchPage(child_pid);
                if (child_frame == nullptr) {
                    bpm_->UnpinPage(item.page_id, false);
                    return {false, "Failed to fetch child page " + std::to_string(child_pid)};
                }
                auto *child_page = BPlusTreePage::Cast(child_frame);
                if (child_page->GetParentPageId() != item.page_id) {
                    page_id_t actual_parent = child_page->GetParentPageId();
                    bpm_->UnpinPage(child_pid, false);
                    bpm_->UnpinPage(item.page_id, false);
                    return {false, "Parent pointer mismatch: child " + std::to_string(child_pid)
                                   + " has parent " + std::to_string(actual_parent) + " but expected " + std::to_string(item.page_id)};
                }
                bpm_->UnpinPage(child_pid, false);

                std::optional<KeyType> child_lower = (i == 0) ? item.lower_bound : std::make_optional(internal->KeyAt(i));
                std::optional<KeyType> child_upper = (i == internal->GetSize() - 1) ? item.upper_bound : std::make_optional(internal->KeyAt(i + 1));
                q.push({child_pid, item.depth + 1, child_lower, child_upper});
            }

            bpm_->UnpinPage(item.page_id, false);
        }
    }

    Page *leftmost_frame = FindLeftmostLeafPage();
    if (leftmost_frame == nullptr) {
        return {false, "FindLeftmostLeafPage failed to find leftmost leaf page"};
    }
    page_id_t curr_leaf_pid = leftmost_frame->GetPageId();
    bpm_->UnpinPage(curr_leaf_pid, false);

    std::set<page_id_t> chain_leaves;
    std::optional<KeyType> prev_max_key;
    size_t chain_key_count = 0;

    while (curr_leaf_pid != INVALID_PAGE_ID) {
        if (chain_leaves.count(curr_leaf_pid)) {
            return {false, "Cycle detected in leaf next-pointer chain at page " + std::to_string(curr_leaf_pid)};
        }
        chain_leaves.insert(curr_leaf_pid);

        Page *leaf_frame = bpm_->FetchPage(curr_leaf_pid);
        if (leaf_frame == nullptr) {
            return {false, "Failed to fetch leaf page " + std::to_string(curr_leaf_pid) + " in chain"};
        }

        auto *leaf = LeafPage::Cast(leaf_frame);
        if (!leaf->IsLeafPage()) {
            bpm_->UnpinPage(curr_leaf_pid, false);
            return {false, "Page " + std::to_string(curr_leaf_pid) + " in leaf chain is not a leaf page"};
        }

        if (leaf->GetSize() > 0) {
            if (prev_max_key.has_value() && Compare(comparator_, *prev_max_key, leaf->KeyAt(0)) >= 0) {
                bpm_->UnpinPage(curr_leaf_pid, false);
                return {false, "Key ordering violation across leaf boundary between previous leaf and leaf " + std::to_string(curr_leaf_pid)};
            }
            prev_max_key = leaf->KeyAt(leaf->GetSize() - 1);
        }

        chain_key_count += leaf->GetSize();
        page_id_t next_pid = leaf->GetNextPageId();
        bpm_->UnpinPage(curr_leaf_pid, false);
        curr_leaf_pid = next_pid;
    }

    if (chain_leaves != hierarchy_leaves) {
        return {false, "Mismatch between leaves found via tree hierarchy (" + std::to_string(hierarchy_leaves.size())
                       + ") and leaf chain (" + std::to_string(chain_leaves.size()) + ")"};
    }

    if (chain_key_count != hierarchy_key_count) {
        return {false, "Key count mismatch between hierarchy (" + std::to_string(hierarchy_key_count)
                       + ") and leaf chain (" + std::to_string(chain_key_count) + ")"};
    }

    if (chain_key_count != GetRecordCount()) {
        return {false, "Record count in header (" + std::to_string(GetRecordCount())
                       + ") does not match indexed keys (" + std::to_string(chain_key_count) + ")"};
    }

    if (observed_leaf_depth != GetDepth()) {
        return {false, "Computed depth (" + std::to_string(observed_leaf_depth)
                       + ") does not match header depth (" + std::to_string(GetDepth()) + ")"};
    }

    return {true, "", chain_key_count, leaf_page_count, internal_page_count, observed_leaf_depth};
}

template <typename KeyType, typename ValueType, typename KeyComparator>
bool BPlusTree<KeyType, ValueType, KeyComparator>::CheckIntegrity(std::string *error_out) const {
    auto res = ValidateTree();
    if (error_out != nullptr && !res.is_valid) {
        *error_out = res.error_message;
    }
    return res.is_valid;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
void BPlusTree<KeyType, ValueType, KeyComparator>::ToDot(std::ostream &out) const {
    std::shared_lock<std::shared_mutex> lock(*rwlock_);

    page_id_t root_pid = GetRootPageId();
    if (root_pid == INVALID_PAGE_ID || bpm_ == nullptr) {
        out << "digraph BPlusTree {\n";
        out << "  labelloc=\"t\";\n";
        out << "  label=\"B+ Tree: " << name_ << " (Empty)\";\n";
        out << "}\n";
        return;
    }

    out << "digraph BPlusTree {\n";
    out << "  rankdir=TB;\n";
    out << "  node [shape=record, fontname=\"Courier\", fontsize=10];\n";
    out << "  edge [fontname=\"Courier\", fontsize=9];\n";
    out << "  labelloc=\"t\";\n";
    out << "  label=\"B+ Tree: " << name_ << " (Depth: " << GetDepth()
        << ", Records: " << GetRecordCount() << ", Root Page: " << root_pid << ")\";\n\n";

    std::queue<page_id_t> q;
    std::vector<page_id_t> leaf_pids;
    std::unordered_set<page_id_t> visited;

    q.push(root_pid);
    visited.insert(root_pid);

    while (!q.empty()) {
        page_id_t pid = q.front();
        q.pop();

        Page *frame = bpm_->FetchPage(pid);
        if (frame == nullptr) continue;

        auto *page = BPlusTreePage::Cast(frame);

        if (page->IsInternalPage()) {
            auto *internal = InternalPage::Cast(frame);

            out << "  page_" << pid << " [label=\"{Page " << pid
                << " | Type: Internal | Size: " << internal->GetSize() << "/" << internal->GetMaxSize()
                << " | Parent: " << internal->GetParentPageId() << "} | {";

            out << "<p0>";
            for (int i = 1; i < internal->GetSize(); ++i) {
                out << " | " << FormatForDot(internal->KeyAt(i)) << " | <p" << i << ">";
            }
            out << "}\", style=filled, fillcolor=\"#E8EEF5\"];\n";

            for (int i = 0; i < internal->GetSize(); ++i) {
                page_id_t child_pid = internal->ValueAt(i);
                out << "  page_" << pid << ":p" << i << " -> page_" << child_pid << ";\n";
                if (visited.insert(child_pid).second) {
                    q.push(child_pid);
                }
            }
        } else if (page->IsLeafPage()) {
            auto *leaf = LeafPage::Cast(frame);
            leaf_pids.push_back(pid);

            out << "  page_" << pid << " [label=\"{Page " << pid
                << " | Type: Leaf | Size: " << leaf->GetSize() << "/" << leaf->GetMaxSize()
                << " | Parent: " << leaf->GetParentPageId()
                << " | Next: " << leaf->GetNextPageId() << "} | {";

            for (int i = 0; i < leaf->GetSize(); ++i) {
                if (i > 0) out << " | ";
                out << FormatForDot(leaf->KeyAt(i)) << ": " << FormatForDot(leaf->ValueAt(i));
            }
            out << "}\", style=filled, fillcolor=\"#E8F5E9\"];\n";
        }

        bpm_->UnpinPage(pid, false);
    }

    out << "\n  // Sequential leaf chain edges\n";
    for (page_id_t leaf_pid : leaf_pids) {
        Page *frame = bpm_->FetchPage(leaf_pid);
        if (frame != nullptr) {
            auto *leaf = LeafPage::Cast(frame);
            page_id_t next_pid = leaf->GetNextPageId();
            if (next_pid != INVALID_PAGE_ID) {
                out << "  page_" << leaf_pid << " -> page_" << next_pid
                    << " [style=dashed, color=\"#2E7D32\", constraint=false];\n";
            }
            bpm_->UnpinPage(leaf_pid, false);
        }
    }

    out << "}\n";
}

template <typename KeyType, typename ValueType, typename KeyComparator>
std::string BPlusTree<KeyType, ValueType, KeyComparator>::ToDot() const {
    std::ostringstream oss;
    ToDot(oss);
    return oss.str();
}

template <typename KeyType, typename ValueType, typename KeyComparator>
bool BPlusTree<KeyType, ValueType, KeyComparator>::DumpGraphviz(const std::string &filename) const {
    std::ofstream fout(filename);
    if (!fout.is_open()) return false;
    ToDot(fout);
    return fout.good();
}

// ==================== Explicit Template Instantiations ====================
template class BPlusTree<int, RID, std::less<int>>;
template class BPlusTree<int, RID, std::greater<int>>;
template class BPlusTree<GenericKey<4>, RID, GenericComparator<4>>;
template class BPlusTree<GenericKey<8>, RID, GenericComparator<8>>;
template class BPlusTree<GenericKey<16>, RID, GenericComparator<16>>;
template class BPlusTree<GenericKey<32>, RID, GenericComparator<32>>;
template class BPlusTree<GenericKey<64>, RID, GenericComparator<64>>;
