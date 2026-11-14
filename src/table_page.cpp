#include "table_page.h"

void TablePage::Init(page_id_t page_id, page_id_t prev_page_id, page_id_t next_page_id) {
    SetPageId(page_id);
    Init(GetData(), page_id, prev_page_id, next_page_id);
    SetDirty(true);
}

void TablePage::Init(char *data, page_id_t page_id, page_id_t prev_page_id, page_id_t next_page_id) {
    std::memset(data, 0, PAGE_SIZE);
    SetTablePageId(data, page_id);
    *reinterpret_cast<uint32_t *>(data + OFFSET_LSN) = 0;
    *reinterpret_cast<page_id_t *>(data + OFFSET_PREV_PAGE_ID) = prev_page_id;
    *reinterpret_cast<page_id_t *>(data + OFFSET_NEXT_PAGE_ID) = next_page_id;
    SetFreeSpacePointer(data, static_cast<uint32_t>(PAGE_SIZE));
    SetNumTuples(data, 0);
}

page_id_t TablePage::GetTablePageId() const {
    return GetTablePageId(GetData());
}

page_id_t TablePage::GetTablePageId(const char *data) {
    return *reinterpret_cast<const page_id_t *>(data + OFFSET_PAGE_ID);
}

void TablePage::SetTablePageId(page_id_t page_id) {
    SetTablePageId(GetData(), page_id);
    SetPageId(page_id);
    SetDirty(true);
}

void TablePage::SetTablePageId(char *data, page_id_t page_id) {
    *reinterpret_cast<page_id_t *>(data + OFFSET_PAGE_ID) = page_id;
}

uint32_t TablePage::GetLSN() const {
    return *reinterpret_cast<const uint32_t *>(GetData() + OFFSET_LSN);
}

void TablePage::SetLSN(uint32_t lsn) {
    *reinterpret_cast<uint32_t *>(GetData() + OFFSET_LSN) = lsn;
    SetDirty(true);
}

page_id_t TablePage::GetPrevPageId() const {
    return *reinterpret_cast<const page_id_t *>(GetData() + OFFSET_PREV_PAGE_ID);
}

void TablePage::SetPrevPageId(page_id_t prev_page_id) {
    *reinterpret_cast<page_id_t *>(GetData() + OFFSET_PREV_PAGE_ID) = prev_page_id;
    SetDirty(true);
}

page_id_t TablePage::GetNextPageId() const {
    return *reinterpret_cast<const page_id_t *>(GetData() + OFFSET_NEXT_PAGE_ID);
}

void TablePage::SetNextPageId(page_id_t next_page_id) {
    *reinterpret_cast<page_id_t *>(GetData() + OFFSET_NEXT_PAGE_ID) = next_page_id;
    SetDirty(true);
}

uint32_t TablePage::GetFreeSpacePointer() const {
    return GetFreeSpacePointer(GetData());
}

uint32_t TablePage::GetFreeSpacePointer(const char *data) {
    return *reinterpret_cast<const uint32_t *>(data + OFFSET_FREE_SPACE);
}

void TablePage::SetFreeSpacePointer(uint32_t free_space_pointer) {
    SetFreeSpacePointer(GetData(), free_space_pointer);
    SetDirty(true);
}

void TablePage::SetFreeSpacePointer(char *data, uint32_t free_space_pointer) {
    if (free_space_pointer > PAGE_SIZE || free_space_pointer < SIZE_TABLE_PAGE_HEADER) {
        return;
    }
    *reinterpret_cast<uint32_t *>(data + OFFSET_FREE_SPACE) = free_space_pointer;
}

uint32_t TablePage::GetNumTuples() const {
    return GetNumTuples(GetData());
}

uint32_t TablePage::GetNumTuples(const char *data) {
    return *reinterpret_cast<const uint32_t *>(data + OFFSET_NUM_TUPLES);
}

void TablePage::SetNumTuples(uint32_t num_tuples) {
    SetNumTuples(GetData(), num_tuples);
    SetDirty(true);
}

void TablePage::SetNumTuples(char *data, uint32_t num_tuples) {
    *reinterpret_cast<uint32_t *>(data + OFFSET_NUM_TUPLES) = num_tuples;
}

uint32_t TablePage::GetFreeSpaceRemaining() const {
    return GetFreeSpaceRemaining(GetData());
}

uint32_t TablePage::GetFreeSpaceRemaining(const char *data) {
    uint32_t free_ptr = GetFreeSpacePointer(data);
    uint32_t num_tuples = GetNumTuples(data);
    uint32_t slot_end = static_cast<uint32_t>(SIZE_TABLE_PAGE_HEADER + num_tuples * SIZEOF_TUPLE_SLOT);
    if (free_ptr < slot_end) {
        return 0;
    }
    return free_ptr - slot_end;
}

uint32_t TablePage::GetTotalFreeSpace() const {
    return GetTotalFreeSpace(GetData());
}

uint32_t TablePage::GetTotalFreeSpace(const char *data) {
    uint32_t num_tuples = GetNumTuples(data);
    uint32_t active_bytes = 0;
    int last_active_slot = -1;

    for (uint32_t i = 0; i < num_tuples; ++i) {
        if (!IsTupleDeleted(data, i) && GetTupleSize(data, i) > 0 && GetTupleOffset(data, i) > 0) {
            active_bytes += GetTupleSize(data, i);
            last_active_slot = static_cast<int>(i);
        }
    }

    uint32_t compacted_num_slots = (last_active_slot == -1) ? 0 : static_cast<uint32_t>(last_active_slot + 1);
    uint32_t used_space = static_cast<uint32_t>(SIZE_TABLE_PAGE_HEADER + compacted_num_slots * SIZEOF_TUPLE_SLOT + active_bytes);

    if (used_space >= PAGE_SIZE) {
        return 0;
    }
    return static_cast<uint32_t>(PAGE_SIZE) - used_space;
}

uint32_t TablePage::GetFragmentedSpace() const {
    return GetFragmentedSpace(GetData());
}

uint32_t TablePage::GetFragmentedSpace(const char *data) {
    uint32_t total_free = GetTotalFreeSpace(data);
    uint32_t contig_free = GetFreeSpaceRemaining(data);
    if (total_free > contig_free) {
        return total_free - contig_free;
    }
    return 0;
}

uint32_t TablePage::GetTupleOffset(uint32_t slot_id) const {
    return GetTupleOffset(GetData(), slot_id);
}

uint32_t TablePage::GetTupleOffset(const char *data, uint32_t slot_id) {
    size_t slot_offset = SIZE_TABLE_PAGE_HEADER + slot_id * SIZEOF_TUPLE_SLOT + OFFSET_SLOT_OFFSET;
    return *reinterpret_cast<const uint32_t *>(data + slot_offset);
}

void TablePage::SetTupleOffset(uint32_t slot_id, uint32_t offset) {
    SetTupleOffset(GetData(), slot_id, offset);
    SetDirty(true);
}

void TablePage::SetTupleOffset(char *data, uint32_t slot_id, uint32_t offset) {
    size_t slot_offset = SIZE_TABLE_PAGE_HEADER + slot_id * SIZEOF_TUPLE_SLOT + OFFSET_SLOT_OFFSET;
    *reinterpret_cast<uint32_t *>(data + slot_offset) = offset;
}

uint32_t TablePage::GetTupleSize(uint32_t slot_id) const {
    return GetTupleSize(GetData(), slot_id);
}

uint32_t TablePage::GetTupleSize(const char *data, uint32_t slot_id) {
    size_t slot_offset = SIZE_TABLE_PAGE_HEADER + slot_id * SIZEOF_TUPLE_SLOT + OFFSET_SLOT_SIZE;
    uint32_t raw_size = *reinterpret_cast<const uint32_t *>(data + slot_offset);
    return raw_size & ~DELETE_MASK;
}

void TablePage::SetTupleSize(uint32_t slot_id, uint32_t size) {
    SetTupleSize(GetData(), slot_id, size);
    SetDirty(true);
}

void TablePage::SetTupleSize(char *data, uint32_t slot_id, uint32_t size) {
    size_t slot_offset = SIZE_TABLE_PAGE_HEADER + slot_id * SIZEOF_TUPLE_SLOT + OFFSET_SLOT_SIZE;
    *reinterpret_cast<uint32_t *>(data + slot_offset) = size;
}

bool TablePage::IsTupleDeleted(uint32_t slot_id) const {
    return IsTupleDeleted(GetData(), slot_id);
}

bool TablePage::IsTupleDeleted(const char *data, uint32_t slot_id) {
    if (slot_id >= GetNumTuples(data)) {
        return true;
    }
    size_t slot_offset = SIZE_TABLE_PAGE_HEADER + slot_id * SIZEOF_TUPLE_SLOT + OFFSET_SLOT_SIZE;
    uint32_t raw_size = *reinterpret_cast<const uint32_t *>(data + slot_offset);
    return (raw_size & DELETE_MASK) != 0 || GetTupleOffset(data, slot_id) == 0 || (raw_size & ~DELETE_MASK) == 0;
}

TupleSlot TablePage::GetSlot(uint32_t slot_id) const {
    return TupleSlot{GetTupleOffset(slot_id), GetTupleSize(slot_id)};
}

void TablePage::SetSlot(uint32_t slot_id, TupleSlot slot) {
    SetTupleOffset(slot_id, slot.offset);
    SetTupleSize(slot_id, slot.size);
}

bool TablePage::InsertTuple(const Tuple &tuple, RID *rid) {
    return InsertTuple(tuple.AsSpan(), rid);
}

bool TablePage::InsertTuple(std::span<const char> tuple_bytes, RID *rid) {
    constexpr size_t MAX_TUPLE_BYTES = PAGE_SIZE - SIZE_TABLE_PAGE_HEADER - SIZEOF_TUPLE_SLOT;
    if (tuple_bytes.empty() || tuple_bytes.size() > MAX_TUPLE_BYTES) {
        return false;
    }
    const uint32_t tuple_len = static_cast<uint32_t>(tuple_bytes.size());

    uint32_t num_tuples = GetNumTuples();

    // Check if we can reuse an empty slot (offset == 0 and size == 0)
    uint32_t slot_id = num_tuples;
    bool reuse_slot = false;
    for (uint32_t i = 0; i < num_tuples; ++i) {
        if (GetTupleOffset(i) == 0 && GetTupleSize(i) == 0) {
            slot_id = i;
            reuse_slot = true;
            break;
        }
    }

    uint32_t needed_space = tuple_len + (reuse_slot ? 0 : SIZEOF_TUPLE_SLOT);
    if (GetFreeSpaceRemaining() < needed_space) {
        // Contiguous free space is insufficient.
        // Check if compaction / defragmentation can reclaim enough space.
        int last_active_slot = -1;
        uint32_t active_bytes = 0;
        for (uint32_t i = 0; i < num_tuples; ++i) {
            if (!IsTupleDeleted(i) && GetTupleSize(i) > 0 && GetTupleOffset(i) > 0) {
                active_bytes += GetTupleSize(i);
                last_active_slot = static_cast<int>(i);
            }
        }
        uint32_t compacted_num_slots = (last_active_slot == -1) ? 0 : static_cast<uint32_t>(last_active_slot + 1);

        bool will_have_reusable_slot = false;
        for (uint32_t i = 0; i < compacted_num_slots; ++i) {
            if (IsTupleDeleted(i) || GetTupleSize(i) == 0 || GetTupleOffset(i) == 0) {
                will_have_reusable_slot = true;
                break;
            }
        }

        uint32_t needed_after_compact = tuple_len + (will_have_reusable_slot ? 0 : SIZEOF_TUPLE_SLOT);
        uint32_t used_after_compact = static_cast<uint32_t>(SIZE_TABLE_PAGE_HEADER + compacted_num_slots * SIZEOF_TUPLE_SLOT + active_bytes);
        uint32_t available_after_compact = (PAGE_SIZE >= used_after_compact) ? (static_cast<uint32_t>(PAGE_SIZE) - used_after_compact) : 0;

        if (available_after_compact < needed_after_compact) {
            return false;
        }

        // Defragment the page to compact free space and reclaim dead/deleted slots
        Defragment();

        // Refresh state after defragmentation
        num_tuples = GetNumTuples();
        slot_id = num_tuples;
        reuse_slot = false;
        for (uint32_t i = 0; i < num_tuples; ++i) {
            if (GetTupleOffset(i) == 0 && GetTupleSize(i) == 0) {
                slot_id = i;
                reuse_slot = true;
                break;
            }
        }

        needed_space = tuple_len + (reuse_slot ? 0 : SIZEOF_TUPLE_SLOT);
        if (GetFreeSpaceRemaining() < needed_space) {
            return false;
        }
    }

    // Allocate tuple at bottom of free space
    uint32_t free_ptr = GetFreeSpacePointer();
    uint32_t target_num_tuples = reuse_slot ? num_tuples : (num_tuples + 1);
    uint32_t slot_array_end = static_cast<uint32_t>(SIZE_TABLE_PAGE_HEADER + target_num_tuples * SIZEOF_TUPLE_SLOT);
    if (free_ptr < tuple_len || free_ptr - tuple_len < slot_array_end) {
        return false;
    }
    free_ptr -= tuple_len;
    SetFreeSpacePointer(free_ptr);

    // Copy data into page
    std::memcpy(GetData() + free_ptr, tuple_bytes.data(), tuple_len);

    // Update slot
    SetTupleOffset(slot_id, free_ptr);
    SetTupleSize(slot_id, tuple_len);

    if (!reuse_slot) {
        SetNumTuples(num_tuples + 1);
    }

    if (rid != nullptr) {
        rid->Set(GetTablePageId(), slot_id);
    }

    SetDirty(true);
    return true;
}

bool TablePage::GetTuple(const RID &rid, Tuple *tuple) const {
    if (rid.GetPageId() == INVALID_PAGE_ID || rid.GetPageId() != GetTablePageId()) {
        return false;
    }
    return GetTuple(GetData(), GetTablePageId(), rid.GetSlotNum(), tuple);
}

bool TablePage::GetTuple(uint32_t slot_id, Tuple *tuple) const {
    return GetTuple(GetData(), GetTablePageId(), slot_id, tuple);
}

std::optional<Tuple> TablePage::GetTuple(const RID &rid) const {
    Tuple tuple;
    if (GetTuple(rid, &tuple)) {
        return tuple;
    }
    return std::nullopt;
}

std::optional<Tuple> TablePage::GetTuple(uint32_t slot_id) const {
    Tuple tuple;
    if (GetTuple(slot_id, &tuple)) {
        return tuple;
    }
    return std::nullopt;
}

bool TablePage::GetTuple(const char *data, page_id_t page_id, uint32_t slot_id, Tuple *tuple) {
    if (slot_id >= GetNumTuples(data)) {
        return false;
    }
    if (IsTupleDeleted(data, slot_id)) {
        return false;
    }

    uint32_t offset = GetTupleOffset(data, slot_id);
    uint32_t size = GetTupleSize(data, slot_id);

    if (offset < SIZE_TABLE_PAGE_HEADER || offset + size > PAGE_SIZE) {
        return false;
    }

    if (tuple != nullptr) {
        *tuple = Tuple(data + offset, size, RID(page_id, slot_id));
    }
    return true;
}

bool TablePage::GetTuple(const char *data, const RID &rid, Tuple *tuple) {
    return GetTuple(data, rid.GetPageId(), rid.GetSlotNum(), tuple);
}

bool TablePage::UpdateTuple(const Tuple &new_tuple, Tuple *old_tuple, const RID &rid) {
    if (rid.GetPageId() == INVALID_PAGE_ID || rid.GetPageId() != GetTablePageId()) {
        return false;
    }
    uint32_t slot_id = rid.GetSlotNum();
    if (slot_id >= GetNumTuples() || IsTupleDeleted(slot_id)) {
        return false;
    }

    uint32_t old_offset = GetTupleOffset(slot_id);
    uint32_t old_size = GetTupleSize(slot_id);

    if (old_tuple != nullptr) {
        *old_tuple = Tuple(GetData() + old_offset, old_size, rid);
    }

    uint32_t new_len = new_tuple.GetLength();

    if (new_len <= old_size) {
        // Fits in-place
        std::memcpy(GetData() + old_offset, new_tuple.GetData(), new_len);
        SetTupleSize(slot_id, new_len);
        SetDirty(true);
        return true;
    }

    // Needs more space
    if (GetFreeSpaceRemaining() < new_len) {
        // Save current tuple contents and temporarily mark slot deleted to assess compaction
        std::vector<char> new_bytes(new_tuple.GetData(), new_tuple.GetData() + new_len);
        SetTupleOffset(slot_id, 0);
        SetTupleSize(slot_id, 0);

        if (GetTotalFreeSpace() < new_len) {
            // Restore old slot info and fail
            SetTupleOffset(slot_id, old_offset);
            SetTupleSize(slot_id, old_size);
            return false;
        }

        Defragment();

        // After defragmentation, allocate new space
        uint32_t free_ptr = GetFreeSpacePointer();
        free_ptr -= new_len;
        SetFreeSpacePointer(free_ptr);

        std::memcpy(GetData() + free_ptr, new_bytes.data(), new_len);
        SetTupleOffset(slot_id, free_ptr);
        SetTupleSize(slot_id, new_len);
        if (slot_id >= GetNumTuples()) {
            SetNumTuples(slot_id + 1);
        }

        SetDirty(true);
        return true;
    }

    // Allocate tuple at bottom of free space
    uint32_t free_ptr = GetFreeSpacePointer();
    uint32_t slot_array_end = static_cast<uint32_t>(SIZE_TABLE_PAGE_HEADER + GetNumTuples() * SIZEOF_TUPLE_SLOT);
    if (free_ptr < new_len || free_ptr - new_len < slot_array_end) {
        return false;
    }
    free_ptr -= new_len;
    SetFreeSpacePointer(free_ptr);

    std::memcpy(GetData() + free_ptr, new_tuple.GetData(), new_len);
    SetTupleOffset(slot_id, free_ptr);
    SetTupleSize(slot_id, new_len);

    SetDirty(true);
    return true;
}

bool TablePage::UpdateTuple(const Tuple &new_tuple, const RID &rid) {
    return UpdateTuple(new_tuple, nullptr, rid);
}

bool TablePage::MarkDelete(const RID &rid) {
    if (rid.GetPageId() == INVALID_PAGE_ID || rid.GetPageId() != GetTablePageId()) {
        return false;
    }
    uint32_t slot_id = rid.GetSlotNum();
    if (slot_id >= GetNumTuples() || IsTupleDeleted(slot_id)) {
        return false;
    }
    char *data = GetData();
    size_t slot_offset = SIZE_TABLE_PAGE_HEADER + slot_id * SIZEOF_TUPLE_SLOT + OFFSET_SLOT_SIZE;
    uint32_t raw_size = *reinterpret_cast<uint32_t *>(data + slot_offset);
    *reinterpret_cast<uint32_t *>(data + slot_offset) = raw_size | DELETE_MASK;
    SetDirty(true);
    return true;
}

bool TablePage::ApplyDelete(const RID &rid) {
    if (rid.GetPageId() == INVALID_PAGE_ID || rid.GetPageId() != GetTablePageId()) {
        return false;
    }
    uint32_t slot_id = rid.GetSlotNum();
    if (slot_id >= GetNumTuples()) {
        return false;
    }
    SetTupleOffset(slot_id, 0);
    SetTupleSize(slot_id, 0);
    SetDirty(true);
    return true;
}

bool TablePage::RollbackDelete(const RID &rid) {
    if (rid.GetPageId() == INVALID_PAGE_ID || rid.GetPageId() != GetTablePageId()) {
        return false;
    }
    uint32_t slot_id = rid.GetSlotNum();
    if (slot_id >= GetNumTuples()) {
        return false;
    }
    char *data = GetData();
    size_t slot_offset = SIZE_TABLE_PAGE_HEADER + slot_id * SIZEOF_TUPLE_SLOT + OFFSET_SLOT_SIZE;
    uint32_t raw_size = *reinterpret_cast<uint32_t *>(data + slot_offset);
    if ((raw_size & DELETE_MASK) == 0) {
        return false; // Not marked deleted
    }
    *reinterpret_cast<uint32_t *>(data + slot_offset) = raw_size & ~DELETE_MASK;
    SetDirty(true);
    return true;
}

bool TablePage::GetFirstTupleRid(RID *first_rid) const {
    uint32_t num = GetNumTuples();
    for (uint32_t i = 0; i < num; ++i) {
        if (!IsTupleDeleted(i) && GetTupleSize(i) > 0 && GetTupleOffset(i) > 0) {
            if (first_rid != nullptr) {
                first_rid->Set(GetTablePageId(), i);
            }
            return true;
        }
    }
    return false;
}

bool TablePage::GetNextTupleRid(const RID &cur_rid, RID *next_rid) const {
    uint32_t num = GetNumTuples();
    for (uint32_t i = cur_rid.GetSlotNum() + 1; i < num; ++i) {
        if (!IsTupleDeleted(i) && GetTupleSize(i) > 0 && GetTupleOffset(i) > 0) {
            if (next_rid != nullptr) {
                next_rid->Set(GetTablePageId(), i);
            }
            return true;
        }
    }
    return false;
}

void TablePage::Defragment() {
    Defragment(GetData());
    SetDirty(true);
}

void TablePage::Defragment(char *data) {
    uint32_t num_tuples = GetNumTuples(data);
    char temp[PAGE_SIZE];
    uint32_t new_free_ptr = static_cast<uint32_t>(PAGE_SIZE);
    const uint32_t slot_array_end = static_cast<uint32_t>(SIZE_TABLE_PAGE_HEADER + num_tuples * SIZEOF_TUPLE_SLOT);

    for (uint32_t i = 0; i < num_tuples; ++i) {
        if (!IsTupleDeleted(data, i) && GetTupleSize(data, i) > 0 && GetTupleOffset(data, i) > 0) {
            uint32_t size = GetTupleSize(data, i);
            uint32_t old_offset = GetTupleOffset(data, i);

            // Boundary validation: prevent tuple data from overlapping the slot metadata array
            if (new_free_ptr < size || (new_free_ptr - size) < slot_array_end) {
                break;
            }
            new_free_ptr -= size;

            std::memcpy(temp + new_free_ptr, data + old_offset, size);
            SetTupleOffset(data, i, new_free_ptr);
        } else {
            SetTupleOffset(data, i, 0);
            SetTupleSize(data, i, 0); // Explicitly writes raw 0, clearing DELETE_MASK
        }
    }

    uint32_t packed_bytes = static_cast<uint32_t>(PAGE_SIZE) - new_free_ptr;
    if (packed_bytes > 0) {
        std::memcpy(data + new_free_ptr, temp + new_free_ptr, packed_bytes);
    }

    SetFreeSpacePointer(data, new_free_ptr);

    // Reclaim trailing deleted slots
    while (num_tuples > 0 && GetTupleOffset(data, num_tuples - 1) == 0 && GetTupleSize(data, num_tuples - 1) == 0) {
        num_tuples--;
    }
    SetNumTuples(data, num_tuples);
}

// ==================== TablePageIterator Implementation ====================

TablePageIterator::TablePageIterator(const TablePage *page, uint32_t slot_id)
    : page_(page), slot_id_(slot_id) {
    if (page_ != nullptr) {
        if (slot_id_ < page_->GetNumTuples()) {
            AdvanceToNextLiveSlot();
        } else {
            slot_id_ = page_->GetNumTuples();
            current_tuple_ = Tuple{};
        }
    } else {
        slot_id_ = 0;
        current_tuple_ = Tuple{};
    }
}

TablePageIterator::TablePageIterator(const TablePage *page, const RID &rid)
    : TablePageIterator(
          page,
          (rid.GetPageId() == INVALID_PAGE_ID || (page != nullptr && rid.GetPageId() == page->GetTablePageId()))
              ? rid.GetSlotNum()
              : (page != nullptr ? page->GetNumTuples() : 0)) {}

void TablePageIterator::AdvanceToNextLiveSlot() {
    if (page_ == nullptr) {
        slot_id_ = 0;
        current_tuple_ = Tuple{};
        return;
    }

    uint32_t num_tuples = page_->GetNumTuples();
    while (slot_id_ < num_tuples) {
        if (!page_->IsTupleDeleted(slot_id_) && page_->GetTupleSize(slot_id_) > 0 && page_->GetTupleOffset(slot_id_) > 0) {
            RID rid(page_->GetTablePageId(), slot_id_);
            Tuple tuple;
            if (page_->GetTuple(rid, &tuple)) {
                current_tuple_ = std::move(tuple);
                return;
            }
        }
        ++slot_id_;
    }

    // Reached end of page
    slot_id_ = num_tuples;
    current_tuple_ = Tuple{};
}

TablePageIterator &TablePageIterator::operator++() {
    if (!IsEnd()) {
        ++slot_id_;
        AdvanceToNextLiveSlot();
    }
    return *this;
}

TablePageIterator TablePageIterator::operator++(int) {
    TablePageIterator temp = *this;
    ++(*this);
    return temp;
}

bool TablePageIterator::operator==(const TablePageIterator &other) const {
    if (IsEnd() && other.IsEnd()) {
        return true;
    }
    if (IsEnd() || other.IsEnd()) {
        return false;
    }
    return page_ == other.page_ && slot_id_ == other.slot_id_;
}

bool TablePageIterator::GetTuple(Tuple *tuple) const {
    if (IsEnd()) {
        return false;
    }
    if (tuple != nullptr) {
        *tuple = current_tuple_;
    }
    return true;
}

RID TablePageIterator::GetRid() const {
    if (IsEnd() || page_ == nullptr) {
        return RID{};
    }
    return RID(page_->GetTablePageId(), slot_id_);
}

page_id_t TablePageIterator::GetPageId() const {
    return page_ != nullptr ? page_->GetTablePageId() : INVALID_PAGE_ID;
}

bool TablePageIterator::IsEnd() const {
    return page_ == nullptr || slot_id_ >= page_->GetNumTuples();
}

bool TablePageIterator::Next(Tuple *tuple, RID *rid) {
    if (IsEnd()) {
        return false;
    }
    if (tuple != nullptr) {
        *tuple = current_tuple_;
    }
    if (rid != nullptr) {
        *rid = GetRid();
    }
    operator++();
    return true;
}

void TablePageIterator::Refresh() {
    if (!IsEnd() && page_ != nullptr) {
        RID rid(page_->GetTablePageId(), slot_id_);
        Tuple tuple;
        if (page_->GetTuple(rid, &tuple)) {
            current_tuple_ = std::move(tuple);
        } else {
            AdvanceToNextLiveSlot();
        }
    }
}

// ==================== TablePage Iterator Methods ====================

TablePage::Iterator TablePage::begin() {
    return Iterator(this, 0);
}

TablePage::Iterator TablePage::end() {
    return Iterator(this, GetNumTuples());
}

TablePage::Iterator TablePage::begin() const {
    return Iterator(this, 0);
}

TablePage::Iterator TablePage::end() const {
    return Iterator(this, GetNumTuples());
}

TablePage::Iterator TablePage::cbegin() const {
    return begin();
}

TablePage::Iterator TablePage::cend() const {
    return end();
}

TablePage::Iterator TablePage::Begin() {
    return begin();
}

TablePage::Iterator TablePage::End() {
    return end();
}

TablePage::Iterator TablePage::Begin() const {
    return begin();
}

TablePage::Iterator TablePage::End() const {
    return end();
}

TablePage::Iterator TablePage::Begin(uint32_t start_slot) const {
    return Iterator(this, start_slot);
}

TablePage::Iterator TablePage::Begin(const RID &start_rid) const {
    return Iterator(this, start_rid);
}

TablePage::Iterator TablePage::GetIterator() const {
    return begin();
}

