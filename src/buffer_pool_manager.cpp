#include "buffer_pool_manager.h"

#include <cstring>
#include <stdexcept>

BufferPoolManager::BufferPoolManager(size_t pool_size, DiskManager *disk_manager, size_t k)
    : pool_size_(pool_size), disk_manager_(disk_manager) {
    if (pool_size_ == 0) {
        throw std::invalid_argument("Buffer pool size must be greater than 0");
    }
    if (disk_manager_ == nullptr) {
        throw std::invalid_argument("DiskManager pointer cannot be null");
    }

    pages_ = std::make_unique<Page[]>(pool_size_);
    replacer_ = std::make_unique<LRUKReplacer>(pool_size_, k);
    frame_io_in_progress_.resize(pool_size_, false);

    for (size_t i = 0; i < pool_size_; ++i) {
        free_list_.push_back(i);
    }
}

BufferPoolManager::BufferPoolManager(size_t pool_size, std::shared_ptr<DiskManager> disk_manager, size_t k)
    : BufferPoolManager(pool_size, disk_manager.get(), k) {
    disk_manager_sp_ = disk_manager;
}

BufferPoolManager::~BufferPoolManager() {
    try {
        FlushAllPages();
    } catch (...) {
        // Destructors must not throw
    }
}

bool BufferPoolManager::FindAvailableFrame(frame_id_t *frame_id) {
    std::unique_lock<std::mutex> lock(latch_);
    return FindAvailableFrame(frame_id, lock);
}

bool BufferPoolManager::FindAvailableFrame(frame_id_t *frame_id, std::unique_lock<std::mutex> &lock) {
    // 1. Try to get a frame from the free list first
    if (!free_list_.empty()) {
        *frame_id = free_list_.front();
        free_list_.pop_front();
        return true;
    }

    // 2. No free frames available: consult the replacer for an evictable victim
    int victim = replacer_->Evict();
    if (victim == -1) {
        // All frames in buffer pool are currently pinned
        return false;
    }

    frame_id_t victim_fid = static_cast<frame_id_t>(victim);
    Page &victim_page = pages_[victim_fid];

    // 3. If the victim page is dirty, write it back to disk without holding the global latch
    if (victim_page.IsDirty()) {
        page_id_t victim_pid = victim_page.GetPageId();
        frame_io_in_progress_[victim_fid] = true;
        victim_page.RLatch();

        // Release global BPM latch during disk write
        lock.unlock();

        bool write_success = false;
        std::exception_ptr ex = nullptr;
        try {
            disk_manager_->WritePage(victim_pid, victim_page.GetData());
            write_success = true;
        } catch (...) {
            ex = std::current_exception();
        }

        victim_page.RUnlatch();

        // Re-acquire global latch
        lock.lock();
        frame_io_in_progress_[victim_fid] = false;
        io_cv_.notify_all();

        if (!write_success) {
            // Restore victim to replacer to prevent frame leak on I/O error
            replacer_->RecordAccess(victim_fid);
            replacer_->SetEvictable(victim_fid, true);
            if (ex) {
                std::rethrow_exception(ex);
            }
            return false;
        }

        victim_page.SetDirty(false);
    }

    // 4. Remove victim page from page_table_ mapping
    page_table_.erase(victim_page.GetPageId());

    *frame_id = victim_fid;
    return true;
}

Page *BufferPoolManager::FetchPage(page_id_t page_id) {
    std::unique_lock<std::mutex> lock(latch_);

    if (page_id == INVALID_PAGE_ID || page_id < 0) {
        return nullptr;
    }

    // 1. Check if the page is already in the buffer pool (or currently being loaded)
    while (true) {
        auto it = page_table_.find(page_id);
        if (it != page_table_.end()) {
            frame_id_t frame_id = it->second;
            Page &page = pages_[frame_id];

            // If another thread is currently performing I/O on this frame,
            // wait on io_cv_ with latch_ released.
            if (frame_io_in_progress_[frame_id]) {
                io_cv_.wait(lock, [&]() { return !frame_io_in_progress_[frame_id]; });
                // Re-evaluate in case I/O failed and frame was unmapped
                continue;
            }

            page.Pin();
            replacer_->RecordAccess(frame_id);
            replacer_->SetEvictable(frame_id, false);
            return &page;
        }
        break;
    }

    // 2. Page miss: find and reserve an available frame
    frame_id_t frame_id;
    if (!FindAvailableFrame(&frame_id, lock)) {
        return nullptr;
    }

    // 3. Frame Reservation: map page_id -> frame_id in page_table_ BEFORE reading from disk!
    Page &page = pages_[frame_id];
    page.SetPageId(page_id);
    page.SetDirty(false);
    page.Pin();
    page_table_[page_id] = frame_id;
    replacer_->RecordAccess(frame_id);
    replacer_->SetEvictable(frame_id, false);

    // Mark frame as having I/O in progress
    frame_io_in_progress_[frame_id] = true;

    // Acquire write latch on the page buffer
    page.WLatch();

    // 4. Release global BPM latch during disk read!
    lock.unlock();

    bool read_success = false;
    std::exception_ptr ex = nullptr;
    try {
        disk_manager_->ReadPage(page_id, page.GetData());
        read_success = true;
    } catch (...) {
        ex = std::current_exception();
    }

    // Release page write latch
    page.WUnlatch();

    // 5. Re-acquire global BPM latch to clear I/O flag and notify waiting threads
    lock.lock();
    frame_io_in_progress_[frame_id] = false;
    io_cv_.notify_all();

    if (!read_success) {
        page_table_.erase(page_id);
        page.SetPageId(INVALID_PAGE_ID);
        page.SetDirty(false);
        page.Unpin();
        free_list_.push_back(frame_id);
        if (ex) {
            std::rethrow_exception(ex);
        }
        return nullptr;
    }

    return &page;
}

bool BufferPoolManager::UnpinPage(page_id_t page_id, bool is_dirty) {
    std::unique_lock<std::mutex> lock(latch_);

    if (page_id == INVALID_PAGE_ID || page_id < 0) {
        return false;
    }

    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;
    }

    frame_id_t frame_id = it->second;
    Page &page = pages_[frame_id];

    if (page.GetPinCount() <= 0) {
        return false;
    }

    if (is_dirty) {
        page.SetDirty(true);
    }

    page.Unpin();

    if (page.GetPinCount() == 0) {
        replacer_->SetEvictable(frame_id, true);
    }

    return true;
}

Page *BufferPoolManager::NewPage(page_id_t *page_id) {
    std::unique_lock<std::mutex> lock(latch_);

    frame_id_t frame_id;
    if (!FindAvailableFrame(&frame_id, lock)) {
        if (page_id != nullptr) {
            *page_id = INVALID_PAGE_ID;
        }
        return nullptr;
    }

    page_id_t new_page_id;
    try {
        new_page_id = disk_manager_->AllocatePage();
    } catch (...) {
        free_list_.push_back(frame_id);
        if (page_id != nullptr) {
            *page_id = INVALID_PAGE_ID;
        }
        throw;
    }

    if (page_id != nullptr) {
        *page_id = new_page_id;
    }

    Page &page = pages_[frame_id];
    page.SetPageId(new_page_id);
    page.SetDirty(false);
    std::memset(page.GetData(), 0, PAGE_SIZE);

    page.Pin();
    page_table_[new_page_id] = frame_id;
    replacer_->RecordAccess(frame_id);
    replacer_->SetEvictable(frame_id, false);

    return &page;
}

bool BufferPoolManager::DeletePage(page_id_t page_id) {
    std::unique_lock<std::mutex> lock(latch_);

    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return true;
    }

    frame_id_t frame_id = it->second;
    Page &page = pages_[frame_id];

    if (page.GetPinCount() > 0 || frame_io_in_progress_[frame_id]) {
        return false;
    }

    // Stop tracking in page_table_ and replacer
    page_table_.erase(it);
    replacer_->SetEvictable(frame_id, false);

    // Reset frame state
    page.SetPageId(INVALID_PAGE_ID);
    page.SetDirty(false);
    std::memset(page.GetData(), 0, PAGE_SIZE);

    free_list_.push_back(frame_id);
    return true;
}

bool BufferPoolManager::FlushPage(page_id_t page_id) {
    std::unique_lock<std::mutex> lock(latch_);

    if (page_id == INVALID_PAGE_ID || page_id < 0) {
        return false;
    }

    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;
    }

    frame_id_t frame_id = it->second;
    Page &page = pages_[frame_id];

    // Wait if an I/O operation is already in progress on this frame
    while (frame_io_in_progress_[frame_id]) {
        io_cv_.wait(lock);
        it = page_table_.find(page_id);
        if (it == page_table_.end()) {
            return false;
        }
    }

    if (!page.IsDirty()) {
        return true;
    }

    frame_io_in_progress_[frame_id] = true;
    page.RLatch();

    // Release global BPM latch during disk write!
    lock.unlock();

    bool write_success = false;
    std::exception_ptr ex = nullptr;
    try {
        disk_manager_->WritePage(page_id, page.GetData());
        write_success = true;
    } catch (...) {
        ex = std::current_exception();
    }

    page.RUnlatch();

    lock.lock();
    frame_io_in_progress_[frame_id] = false;
    io_cv_.notify_all();

    if (write_success) {
        page.SetDirty(false);
        return true;
    } else {
        if (ex) {
            std::rethrow_exception(ex);
        }
        return false;
    }
}

void BufferPoolManager::FlushAllPages() {
    std::unique_lock<std::mutex> lock(latch_);

    std::vector<page_id_t> page_ids;
    page_ids.reserve(page_table_.size());
    for (const auto &[page_id, frame_id] : page_table_) {
        page_ids.push_back(page_id);
    }

    for (page_id_t page_id : page_ids) {
        auto it = page_table_.find(page_id);
        if (it == page_table_.end()) {
            continue;
        }
        frame_id_t frame_id = it->second;
        Page &page = pages_[frame_id];

        while (frame_io_in_progress_[frame_id]) {
            io_cv_.wait(lock);
            it = page_table_.find(page_id);
            if (it == page_table_.end()) {
                break;
            }
        }
        if (it == page_table_.end()) {
            continue;
        }

        if (page.IsDirty()) {
            frame_io_in_progress_[frame_id] = true;
            page.RLatch();

            lock.unlock();

            bool write_success = false;
            std::exception_ptr ex = nullptr;
            try {
                disk_manager_->WritePage(page_id, page.GetData());
                write_success = true;
            } catch (...) {
                ex = std::current_exception();
            }

            page.RUnlatch();

            lock.lock();
            frame_io_in_progress_[frame_id] = false;
            io_cv_.notify_all();

            if (write_success) {
                page.SetDirty(false);
            } else if (ex) {
                std::rethrow_exception(ex);
            }
        }
    }
}

size_t BufferPoolManager::GetFreeListSize() const {
    std::lock_guard<std::mutex> lock(latch_);
    return free_list_.size();
}

bool BufferPoolManager::IsPageInPool(page_id_t page_id) const {
    std::lock_guard<std::mutex> lock(latch_);
    return page_table_.find(page_id) != page_table_.end();
}

frame_id_t BufferPoolManager::GetFrameId(page_id_t page_id) const {
    std::lock_guard<std::mutex> lock(latch_);
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        return it->second;
    }
    return INVALID_FRAME_ID;
}

Page *BufferPoolManager::GetPage(frame_id_t frame_id) {
    if (frame_id < pool_size_) {
        return &pages_[frame_id];
    }
    return nullptr;
}

const Page *BufferPoolManager::GetPage(frame_id_t frame_id) const {
    if (frame_id < pool_size_) {
        return &pages_[frame_id];
    }
    return nullptr;
}
