#include "b_plus_tree_header_page.h"

void BPlusTreeHeaderPage::Init(page_id_t header_page_id, page_id_t root_page_id, uint32_t key_size) {
    page_type_ = PageType::BPLUS_TREE_HEADER_PAGE;
    lsn_ = 0;
    root_page_id_ = root_page_id;
    header_page_id_ = header_page_id;
    key_size_ = key_size;
    depth_ = (root_page_id == INVALID_PAGE_ID) ? 0 : 1;
    record_count_ = 0;
}

BPlusTreeHeaderPage *BPlusTreeHeaderPage::Cast(Page *page) {
    if (page == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<BPlusTreeHeaderPage *>(page->GetData());
}

const BPlusTreeHeaderPage *BPlusTreeHeaderPage::Cast(const Page *page) {
    if (page == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<const BPlusTreeHeaderPage *>(page->GetData());
}

page_id_t BPlusTreeHeaderPage::GetRootPageId() const {
    return root_page_id_;
}

void BPlusTreeHeaderPage::SetRootPageId(page_id_t root_page_id) {
    root_page_id_ = root_page_id;
    if (root_page_id == INVALID_PAGE_ID) {
        depth_ = 0;
    } else if (depth_ == 0) {
        depth_ = 1;
    }
}

bool BPlusTreeHeaderPage::HasRoot() const {
    return root_page_id_ != INVALID_PAGE_ID;
}

page_id_t BPlusTreeHeaderPage::GetHeaderPageId() const {
    return header_page_id_;
}

void BPlusTreeHeaderPage::SetHeaderPageId(page_id_t header_page_id) {
    header_page_id_ = header_page_id;
}

PageType BPlusTreeHeaderPage::GetPageType() const {
    return page_type_;
}

lsn_t BPlusTreeHeaderPage::GetLSN() const {
    return lsn_;
}

void BPlusTreeHeaderPage::SetLSN(lsn_t lsn) {
    lsn_ = lsn;
}

uint32_t BPlusTreeHeaderPage::GetKeySize() const {
    return key_size_;
}

void BPlusTreeHeaderPage::SetKeySize(uint32_t key_size) {
    key_size_ = key_size;
}

uint32_t BPlusTreeHeaderPage::GetDepth() const {
    return depth_;
}

void BPlusTreeHeaderPage::SetDepth(uint32_t depth) {
    depth_ = depth;
}

uint64_t BPlusTreeHeaderPage::GetRecordCount() const {
    return record_count_;
}

void BPlusTreeHeaderPage::SetRecordCount(uint64_t count) {
    record_count_ = count;
}

void BPlusTreeHeaderPage::IncreaseRecordCount(int64_t amount) {
    record_count_ += amount;
}

