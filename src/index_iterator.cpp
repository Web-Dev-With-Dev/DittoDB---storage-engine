#include "index_iterator.h"
#include "buffer_pool_manager.h"
#include "generic_key.h"
#include <functional>

template <typename KeyType, typename ValueType, typename KeyComparator>
IndexIterator<KeyType, ValueType, KeyComparator>::IndexIterator(BufferPoolManager *bpm, Page *page, int index)
    : bpm_(bpm), page_(page), page_id_(page != nullptr ? page->GetPageId() : INVALID_PAGE_ID), index_(index) {
    if (page_ != nullptr) {
        auto *leaf = LeafPage::Cast(page_);
        if (index_ >= leaf->GetSize()) {
            AdvanceToNextValidLeaf();
        }
    }
}

template <typename KeyType, typename ValueType, typename KeyComparator>
IndexIterator<KeyType, ValueType, KeyComparator>::~IndexIterator() {
    ReleaseCurrentPage();
}

template <typename KeyType, typename ValueType, typename KeyComparator>
IndexIterator<KeyType, ValueType, KeyComparator>::IndexIterator(const IndexIterator &other)
    : bpm_(other.bpm_), page_(other.page_), page_id_(other.page_id_), index_(other.index_) {
    if (bpm_ != nullptr && page_ != nullptr && page_id_ != INVALID_PAGE_ID) {
        bpm_->FetchPage(page_id_);
    }
}

template <typename KeyType, typename ValueType, typename KeyComparator>
IndexIterator<KeyType, ValueType, KeyComparator> &IndexIterator<KeyType, ValueType, KeyComparator>::operator=(const IndexIterator &other) {
    if (this != &other) {
        ReleaseCurrentPage();
        bpm_ = other.bpm_;
        page_ = other.page_;
        page_id_ = other.page_id_;
        index_ = other.index_;
        if (bpm_ != nullptr && page_ != nullptr && page_id_ != INVALID_PAGE_ID) {
            bpm_->FetchPage(page_id_);
        }
    }
    return *this;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
IndexIterator<KeyType, ValueType, KeyComparator>::IndexIterator(IndexIterator &&other) noexcept
    : bpm_(other.bpm_), page_(other.page_), page_id_(other.page_id_), index_(other.index_) {
    other.page_ = nullptr;
    other.page_id_ = INVALID_PAGE_ID;
    other.index_ = 0;
    other.bpm_ = nullptr;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
IndexIterator<KeyType, ValueType, KeyComparator> &IndexIterator<KeyType, ValueType, KeyComparator>::operator=(IndexIterator &&other) noexcept {
    if (this != &other) {
        ReleaseCurrentPage();
        bpm_ = other.bpm_;
        page_ = other.page_;
        page_id_ = other.page_id_;
        index_ = other.index_;
        other.page_ = nullptr;
        other.page_id_ = INVALID_PAGE_ID;
        other.index_ = 0;
        other.bpm_ = nullptr;
    }
    return *this;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
bool IndexIterator<KeyType, ValueType, KeyComparator>::IsEnd() const {
    return page_ == nullptr || page_id_ == INVALID_PAGE_ID;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
IndexIterator<KeyType, ValueType, KeyComparator>::operator bool() const {
    return !IsEnd();
}

template <typename KeyType, typename ValueType, typename KeyComparator>
const typename IndexIterator<KeyType, ValueType, KeyComparator>::MappingType &
IndexIterator<KeyType, ValueType, KeyComparator>::operator*() const {
    assert(!IsEnd() && page_ != nullptr && index_ >= 0);
    const auto *leaf = LeafPage::Cast(page_);
    return leaf->ItemAt(index_);
}

template <typename KeyType, typename ValueType, typename KeyComparator>
const typename IndexIterator<KeyType, ValueType, KeyComparator>::MappingType *
IndexIterator<KeyType, ValueType, KeyComparator>::operator->() const {
    assert(!IsEnd() && page_ != nullptr && index_ >= 0);
    const auto *leaf = LeafPage::Cast(page_);
    return &leaf->ItemAt(index_);
}

template <typename KeyType, typename ValueType, typename KeyComparator>
IndexIterator<KeyType, ValueType, KeyComparator> &IndexIterator<KeyType, ValueType, KeyComparator>::operator++() {
    if (IsEnd() || page_ == nullptr) {
        return *this;
    }
    index_++;
    auto *leaf = LeafPage::Cast(page_);
    if (index_ >= leaf->GetSize()) {
        AdvanceToNextValidLeaf();
    }
    return *this;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
IndexIterator<KeyType, ValueType, KeyComparator> IndexIterator<KeyType, ValueType, KeyComparator>::operator++(int) {
    IndexIterator temp = *this;
    ++(*this);
    return temp;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
bool IndexIterator<KeyType, ValueType, KeyComparator>::operator==(const IndexIterator &other) const {
    if (IsEnd() && other.IsEnd()) {
        return true;
    }
    if (IsEnd() || other.IsEnd()) {
        return false;
    }
    return page_id_ == other.page_id_ && index_ == other.index_;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
bool IndexIterator<KeyType, ValueType, KeyComparator>::operator!=(const IndexIterator &other) const {
    return !(*this == other);
}

template <typename KeyType, typename ValueType, typename KeyComparator>
void IndexIterator<KeyType, ValueType, KeyComparator>::ReleaseCurrentPage() {
    if (bpm_ != nullptr && page_ != nullptr && page_id_ != INVALID_PAGE_ID) {
        bpm_->UnpinPage(page_id_, false);
        page_ = nullptr;
        page_id_ = INVALID_PAGE_ID;
        index_ = 0;
    }
}

template <typename KeyType, typename ValueType, typename KeyComparator>
void IndexIterator<KeyType, ValueType, KeyComparator>::AdvanceToNextValidLeaf() {
    while (page_ != nullptr) {
        auto *leaf = LeafPage::Cast(page_);
        page_id_t next_page_id = leaf->GetNextPageId();
        ReleaseCurrentPage();

        if (next_page_id == INVALID_PAGE_ID || bpm_ == nullptr) {
            break;
        }

        Page *next_page = bpm_->FetchPage(next_page_id);
        if (next_page == nullptr) {
            break;
        }

        page_ = next_page;
        page_id_ = next_page_id;
        index_ = 0;

        auto *next_leaf = LeafPage::Cast(page_);
        if (next_leaf->GetSize() > 0) {
            break;
        }
    }
}

// ==================== Explicit Template Instantiations ====================
template class IndexIterator<int, RID, std::less<int>>;
template class IndexIterator<int, RID, std::greater<int>>;
template class IndexIterator<GenericKey<4>, RID, GenericComparator<4>>;
template class IndexIterator<GenericKey<8>, RID, GenericComparator<8>>;
template class IndexIterator<GenericKey<16>, RID, GenericComparator<16>>;
template class IndexIterator<GenericKey<32>, RID, GenericComparator<32>>;
template class IndexIterator<GenericKey<64>, RID, GenericComparator<64>>;

