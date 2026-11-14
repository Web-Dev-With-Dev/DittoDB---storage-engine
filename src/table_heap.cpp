#include "table_page.h"
#include "page.h"
#include "table_heap.h"
#include <algorithm>

// ==================== TableIterator Implementation ====================

TableIterator::TableIterator(const TableHeap *table_heap, const RID &rid)
    : table_heap_(table_heap), rid_(rid) {
    if (table_heap_ == nullptr || !rid_.IsValid()) {
        rid_ = RID{};
        tuple_ = Tuple{};
        return;
    }

    // If rid_ points to an active live tuple, load it directly
    if (table_heap_->GetTuple(rid_, &tuple_)) {
        return;
    }

    // Otherwise advance to the next live tuple at or after rid_
    AdvanceToNextLiveTuple();
}

void TableIterator::AdvanceToNextLiveTuple() {
    if (table_heap_ == nullptr || table_heap_->GetBufferPoolManager() == nullptr) {
        rid_ = RID{};
        tuple_ = Tuple{};
        return;
    }

    BufferPoolManager *bpm = table_heap_->GetBufferPoolManager();
    page_id_t cur_page_id = rid_.IsValid() ? rid_.GetPageId() : table_heap_->GetFirstPageId();
    bool check_current_page_next = rid_.IsValid();

    while (cur_page_id != INVALID_PAGE_ID) {
        Page *raw_page = bpm->FetchPage(cur_page_id);
        if (raw_page == nullptr) {
            rid_ = RID{};
            tuple_ = Tuple{};
            return;
        }

        const TablePage *table_page = TablePage::Cast(raw_page);
        RID next_rid;
        bool found = false;

        if (check_current_page_next && rid_.GetPageId() == cur_page_id) {
            found = table_page->GetNextTupleRid(rid_, &next_rid);
            check_current_page_next = false;
        } else {
            found = table_page->GetFirstTupleRid(&next_rid);
        }

        if (found) {
            table_page->GetTuple(next_rid, &tuple_);
            rid_ = next_rid;
            bpm->UnpinPage(cur_page_id, false);
            return;
        }

        // No more live tuples on this page, advance to the next page in the chain
        page_id_t next_page_id = table_page->GetNextPageId();
        bpm->UnpinPage(cur_page_id, false);
        cur_page_id = next_page_id;
    }

    // Reached end of page chain
    rid_ = RID{};
    tuple_ = Tuple{};
}

TableIterator &TableIterator::operator++() {
    if (IsEnd()) {
        return *this;
    }
    AdvanceToNextLiveTuple();
    return *this;
}

TableIterator TableIterator::operator++(int) {
    TableIterator temp = *this;
    ++(*this);
    return temp;
}

bool TableIterator::operator==(const TableIterator &other) const {
    if (IsEnd() && other.IsEnd()) {
        return true;
    }
    return table_heap_ == other.table_heap_ && rid_ == other.rid_;
}

bool TableIterator::Next(Tuple *tuple, RID *rid) {
    if (IsEnd()) {
        return false;
    }
    if (tuple != nullptr) {
        *tuple = tuple_;
    }
    if (rid != nullptr) {
        *rid = rid_;
    }
    operator++();
    return true;
}

// ==================== TableHeap Implementation ====================

TableHeap::TableHeap(BufferPoolManager *bpm, page_id_t first_page_id)
    : bpm_(bpm), first_page_id_(first_page_id) {
    if (first_page_id_ == INVALID_PAGE_ID && bpm_ != nullptr) {
        Page *first_page = bpm_->NewPage(&first_page_id_);
        if (first_page != nullptr) {
            TablePage *table_page = TablePage::Cast(first_page);
            table_page->Init(first_page_id_, INVALID_PAGE_ID, INVALID_PAGE_ID);
            bpm_->UnpinPage(first_page_id_, true);
        }
    }
}

TableHeap::TableHeap(std::shared_ptr<BufferPoolManager> bpm, page_id_t first_page_id)
    : TableHeap(bpm.get(), first_page_id) {
    bpm_sp_ = bpm;
}

bool TableHeap::InsertTuple(const Tuple &tuple, RID *rid) {
    std::lock_guard<std::mutex> guard(latch_);

    if (bpm_ == nullptr) {
        return false;
    }

    const uint32_t tuple_len = tuple.GetLength();
    if (tuple_len == 0 || tuple_len > PAGE_SIZE - TablePage::SIZE_TABLE_PAGE_HEADER - TablePage::SIZEOF_TUPLE_SLOT) {
        return false;
    }

    if (first_page_id_ == INVALID_PAGE_ID) {
        Page *raw_page = bpm_->NewPage(&first_page_id_);
        if (raw_page == nullptr) {
            return false;
        }
        TablePage *table_page = TablePage::Cast(raw_page);
        table_page->Init(first_page_id_, INVALID_PAGE_ID, INVALID_PAGE_ID);
        bool ok = table_page->InsertTuple(tuple, rid);
        bpm_->UnpinPage(first_page_id_, true);
        return ok;
    }

    page_id_t cur_page_id = first_page_id_;
    while (cur_page_id != INVALID_PAGE_ID) {
        Page *raw_page = bpm_->FetchPage(cur_page_id);
        if (raw_page == nullptr) {
            return false;
        }

        TablePage *table_page = TablePage::Cast(raw_page);
        if (table_page->InsertTuple(tuple, rid)) {
            bpm_->UnpinPage(cur_page_id, true);
            return true;
        }

        // Current page is full, check next page in chain
        page_id_t next_page_id = table_page->GetNextPageId();
        if (next_page_id == INVALID_PAGE_ID) {
            // At the end of the chain, allocate and link a new page
            page_id_t new_page_id = INVALID_PAGE_ID;
            Page *new_raw_page = bpm_->NewPage(&new_page_id);
            if (new_raw_page == nullptr) {
                bpm_->UnpinPage(cur_page_id, false);
                return false;
            }

            TablePage *new_table_page = TablePage::Cast(new_raw_page);
            new_table_page->Init(new_page_id, cur_page_id, INVALID_PAGE_ID);

            table_page->SetNextPageId(new_page_id);
            bpm_->UnpinPage(cur_page_id, true);

            bool ok = new_table_page->InsertTuple(tuple, rid);
            bpm_->UnpinPage(new_page_id, true);
            return ok;
        }

        bpm_->UnpinPage(cur_page_id, false);
        cur_page_id = next_page_id;
    }

    return false;
}

bool TableHeap::GetTuple(const RID &rid, Tuple *tuple) const {
    if (!rid.IsValid() || bpm_ == nullptr) {
        return false;
    }

    Page *raw_page = bpm_->FetchPage(rid.GetPageId());
    if (raw_page == nullptr) {
        return false;
    }

    const TablePage *table_page = TablePage::Cast(raw_page);
    bool success = table_page->GetTuple(rid, tuple);
    bpm_->UnpinPage(rid.GetPageId(), false);
    return success;
}

std::optional<Tuple> TableHeap::GetTuple(const RID &rid) const {
    Tuple tuple;
    if (GetTuple(rid, &tuple)) {
        return tuple;
    }
    return std::nullopt;
}

bool TableHeap::UpdateTuple(const Tuple &new_tuple, Tuple *old_tuple, const RID &rid, RID *new_rid) {
    std::lock_guard<std::mutex> guard(latch_);

    if (!rid.IsValid() || bpm_ == nullptr) {
        return false;
    }

    const uint32_t new_len = new_tuple.GetLength();
    if (new_len == 0 || new_len > PAGE_SIZE - TablePage::SIZE_TABLE_PAGE_HEADER - TablePage::SIZEOF_TUPLE_SLOT) {
        return false;
    }

    Page *raw_page = bpm_->FetchPage(rid.GetPageId());
    if (raw_page == nullptr) {
        return false;
    }

    TablePage *table_page = TablePage::Cast(raw_page);

    // 1. Try in-place / in-page update first
    if (table_page->UpdateTuple(new_tuple, old_tuple, rid)) {
        bpm_->UnpinPage(rid.GetPageId(), true);
        if (new_rid != nullptr) {
            *new_rid = rid; // In-place, same RID!
        }
        return true;
    }

    // 2. In-page update failed because the page cannot fit the larger tuple.
    // Execute Strategy 3: Delete Old + Insert New on the heap.
    if (old_tuple != nullptr && old_tuple->IsEmpty()) {
        table_page->GetTuple(rid, old_tuple);
    }

    // Mark delete on the old page
    if (!table_page->MarkDelete(rid)) {
        bpm_->UnpinPage(rid.GetPageId(), false);
        return false;
    }
    bpm_->UnpinPage(rid.GetPageId(), true);

    // 3. Insert new tuple into the heap (searches for space or allocates a new linked page)
    // Note: InsertTuple acquires latch_, but we are already holding latch_.
    // To allow re-entrant insertion within UpdateTuple, we perform page traversal directly here.
    page_id_t cur_page_id = first_page_id_;
    while (cur_page_id != INVALID_PAGE_ID) {
        Page *p = bpm_->FetchPage(cur_page_id);
        if (p == nullptr) {
            break;
        }

        TablePage *tp = TablePage::Cast(p);
        RID allocated_rid;
        if (tp->InsertTuple(new_tuple, &allocated_rid)) {
            bpm_->UnpinPage(cur_page_id, true);
            if (new_rid != nullptr) {
                *new_rid = allocated_rid;
            }
            return true;
        }

        page_id_t next_page_id = tp->GetNextPageId();
        if (next_page_id == INVALID_PAGE_ID) {
            page_id_t new_page_id = INVALID_PAGE_ID;
            Page *new_p = bpm_->NewPage(&new_page_id);
            if (new_p == nullptr) {
                bpm_->UnpinPage(cur_page_id, false);
                break;
            }

            TablePage *new_tp = TablePage::Cast(new_p);
            new_tp->Init(new_page_id, cur_page_id, INVALID_PAGE_ID);
            tp->SetNextPageId(new_page_id);
            bpm_->UnpinPage(cur_page_id, true);

            bool ok = new_tp->InsertTuple(new_tuple, &allocated_rid);
            bpm_->UnpinPage(new_page_id, true);

            if (ok) {
                if (new_rid != nullptr) {
                    *new_rid = allocated_rid;
                }
                return true;
            }
            break;
        }

        bpm_->UnpinPage(cur_page_id, false);
        cur_page_id = next_page_id;
    }

    // Insertion failed across all pages, rollback the delete on the old page
    Page *rollback_page = bpm_->FetchPage(rid.GetPageId());
    if (rollback_page != nullptr) {
        TablePage::Cast(rollback_page)->RollbackDelete(rid);
        bpm_->UnpinPage(rid.GetPageId(), true);
    }
    return false;
}

bool TableHeap::UpdateTuple(const Tuple &new_tuple, const RID &rid, RID *new_rid) {
    return UpdateTuple(new_tuple, nullptr, rid, new_rid);
}

bool TableHeap::MarkDelete(const RID &rid) {
    std::lock_guard<std::mutex> guard(latch_);

    if (!rid.IsValid() || bpm_ == nullptr) {
        return false;
    }

    Page *raw_page = bpm_->FetchPage(rid.GetPageId());
    if (raw_page == nullptr) {
        return false;
    }

    TablePage *table_page = TablePage::Cast(raw_page);
    bool success = table_page->MarkDelete(rid);
    bpm_->UnpinPage(rid.GetPageId(), success);
    return success;
}

bool TableHeap::ApplyDelete(const RID &rid) {
    std::lock_guard<std::mutex> guard(latch_);

    if (!rid.IsValid() || bpm_ == nullptr) {
        return false;
    }

    Page *raw_page = bpm_->FetchPage(rid.GetPageId());
    if (raw_page == nullptr) {
        return false;
    }

    TablePage *table_page = TablePage::Cast(raw_page);
    bool success = table_page->ApplyDelete(rid);
    bpm_->UnpinPage(rid.GetPageId(), success);
    return success;
}

bool TableHeap::RollbackDelete(const RID &rid) {
    std::lock_guard<std::mutex> guard(latch_);

    if (!rid.IsValid() || bpm_ == nullptr) {
        return false;
    }

    Page *raw_page = bpm_->FetchPage(rid.GetPageId());
    if (raw_page == nullptr) {
        return false;
    }

    TablePage *table_page = TablePage::Cast(raw_page);
    bool success = table_page->RollbackDelete(rid);
    bpm_->UnpinPage(rid.GetPageId(), success);
    return success;
}

TableIterator TableHeap::begin() {
    if (first_page_id_ == INVALID_PAGE_ID || bpm_ == nullptr) {
        return end();
    }
    return TableIterator(this, RID(first_page_id_, 0));
}

TableIterator TableHeap::end() {
    return TableIterator(this, RID{});
}

TableIterator TableHeap::begin() const {
    if (first_page_id_ == INVALID_PAGE_ID || bpm_ == nullptr) {
        return end();
    }
    return TableIterator(this, RID(first_page_id_, 0));
}

TableIterator TableHeap::end() const {
    return TableIterator(this, RID{});
}

TableIterator TableHeap::cbegin() const {
    return begin();
}

TableIterator TableHeap::cend() const {
    return end();
}

std::vector<page_id_t> TableHeap::GetPageIds() const {
    std::vector<page_id_t> page_ids;
    if (bpm_ == nullptr || first_page_id_ == INVALID_PAGE_ID) {
        return page_ids;
    }

    page_id_t cur = first_page_id_;
    while (cur != INVALID_PAGE_ID) {
        page_ids.push_back(cur);
        Page *raw = bpm_->FetchPage(cur);
        if (raw == nullptr) {
            break;
        }
        const TablePage *tp = TablePage::Cast(raw);
        page_id_t next = tp->GetNextPageId();
        bpm_->UnpinPage(cur, false);
        cur = next;
    }
    return page_ids;
}

size_t TableHeap::GetTupleCount() const {
    size_t count = 0;
    if (bpm_ == nullptr || first_page_id_ == INVALID_PAGE_ID) {
        return count;
    }

    page_id_t cur = first_page_id_;
    while (cur != INVALID_PAGE_ID) {
        Page *raw = bpm_->FetchPage(cur);
        if (raw == nullptr) {
            break;
        }
        const TablePage *tp = TablePage::Cast(raw);
        RID r;
        if (tp->GetFirstTupleRid(&r)) {
            count++;
            while (tp->GetNextTupleRid(r, &r)) {
                count++;
            }
        }
        page_id_t next = tp->GetNextPageId();
        bpm_->UnpinPage(cur, false);
        cur = next;
    }
    return count;
}
