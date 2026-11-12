#include "lru_k_replacer.h"
#include <stdexcept>

LRUKReplacer::LRUKReplacer(size_t num_frames, size_t k)
    : total_frames_(num_frames), k_(k), current_time_(0), eviction_count_(0) {
    
    if (num_frames <= 0) {
        throw std::invalid_argument("Number of frames must be greater than 0");
    }
    
    if (k <= 0) {
        throw std::invalid_argument("K must be greater than 0");
    }
    
    frame_info_.resize(num_frames);
    for (size_t i = 0; i < num_frames; ++i) {
        frame_info_[i].frame_id = i;
    }
}

void LRUKReplacer::RecordAccess(size_t frame_id) {
    std::lock_guard<std::mutex> lock(replacer_latch_);
    if (frame_id >= total_frames_) {
        throw std::out_of_range("Frame ID out of range");
    }

    FrameInfo& info = frame_info_[frame_id];
    current_time_++;
    
    info.access_history.push_back(current_time_);
    info.access_count++;
    
    while (info.access_history.size() > k_) {
        info.access_history.pop_front();
    }
    
    if (info.is_evictable && !info.is_pinned) {
        RemoveFromQueue(frame_id);
        AddToQueue(frame_id);
    }
}

void LRUKReplacer::Pin(size_t frame_id) {
    std::lock_guard<std::mutex> lock(replacer_latch_);
    if (frame_id >= total_frames_) {
        throw std::out_of_range("Frame ID out of range");
    }
    
    FrameInfo& info = frame_info_[frame_id];
    info.is_pinned = true;
    info.is_evictable = false;
    
    RemoveFromQueue(frame_id);
}

void LRUKReplacer::Unpin(size_t frame_id) {
    std::lock_guard<std::mutex> lock(replacer_latch_);
    if (frame_id >= total_frames_) {
        throw std::out_of_range("Frame ID out of range");
    }
    
    FrameInfo& info = frame_info_[frame_id];
    if (!info.is_pinned) {
        return;
    }

    info.is_pinned = false;
    // Only mark evictable (and queue) if the frame has been accessed at least once.
    // A never-accessed frame has no meaningful LRU-K history to evict by.
    if (info.access_count > 0) {
        info.is_evictable = true;
        AddToQueue(frame_id);
    }
}

int LRUKReplacer::Evict() {
    std::lock_guard<std::mutex> lock(replacer_latch_);
    // Check cold queue first (frames with < k accesses)
    if (!cold_queue_.empty()) {
        int frame_id = cold_queue_.front();
        cold_queue_.pop_front();
        position_in_cold_.erase(frame_id);
        
        // Reset frame info
        FrameInfo& info = frame_info_[frame_id];
        info.access_history.clear();
        info.access_count = 0;
        info.is_evictable = false;
        eviction_count_++;
        
        return frame_id;
    }
    
    // Check hot queue (frames with >= k accesses)
    if (!hot_queue_.empty()) {
        int frame_id = hot_queue_.front();
        hot_queue_.pop_front();
        position_in_hot_.erase(frame_id);
        
        // Reset frame info
        FrameInfo& info = frame_info_[frame_id];
        info.access_history.clear();
        info.access_count = 0;
        info.is_evictable = false;
        eviction_count_++;
        
        return frame_id;
    }
    
    // No evictable frames
    return -1;
}

void LRUKReplacer::SetEvictable(size_t frame_id, bool evictable) {
    std::lock_guard<std::mutex> lock(replacer_latch_);
    if (frame_id >= total_frames_) {
        throw std::out_of_range("Frame ID out of range");
    }
    
    FrameInfo& info = frame_info_[frame_id];
    
    if (evictable) {
        // SetEvictable(true) is an explicit override: clear pin flag and enqueue.
        // Only meaningful if the frame has been accessed at least once.
        info.is_pinned = false;
        if (info.access_count > 0) {
            info.is_evictable = true;
            AddToQueue(frame_id);
        }
    } else {
        // SetEvictable(false): frame must not be evicted.
        info.is_pinned = true;
        info.is_evictable = false;
        RemoveFromQueue(frame_id);
    }
}

size_t LRUKReplacer::GetEvictableCount() const {
    std::lock_guard<std::mutex> lock(replacer_latch_);
    return cold_queue_.size() + hot_queue_.size();
}

size_t LRUKReplacer::GetEvictionCount() const {
    std::lock_guard<std::mutex> lock(replacer_latch_);
    return eviction_count_;
}

size_t LRUKReplacer::GetTotalFrames() const {
    return total_frames_;
}

bool LRUKReplacer::IsEvictable(size_t frame_id) const {
    std::lock_guard<std::mutex> lock(replacer_latch_);
    if (frame_id >= total_frames_) {
        return false;
    }
    return frame_info_[frame_id].is_evictable;
}

bool LRUKReplacer::IsPinned(size_t frame_id) const {
    std::lock_guard<std::mutex> lock(replacer_latch_);
    if (frame_id >= total_frames_) {
        return false;
    }
    return frame_info_[frame_id].is_pinned;
}

size_t LRUKReplacer::GetAccessCount(size_t frame_id) const {
    std::lock_guard<std::mutex> lock(replacer_latch_);
    if (frame_id >= total_frames_) {
        return 0;
    }
    return frame_info_[frame_id].access_count;
}

void LRUKReplacer::Reset() {
    std::lock_guard<std::mutex> lock(replacer_latch_);
    cold_queue_.clear();
    position_in_cold_.clear();
    hot_queue_.clear();
    position_in_hot_.clear();
    
    for (auto& info : frame_info_) {
        info.access_history.clear();
        info.access_count = 0;
        info.is_pinned = false;
        info.is_evictable = false;
    }
    
    current_time_ = 0;
    eviction_count_ = 0;
}

void LRUKReplacer::AddToQueue(size_t frame_id) {
    FrameInfo& info = frame_info_[frame_id];
    
    if (!info.is_evictable || info.is_pinned) {
        return;
    }
    
    RemoveFromQueue(frame_id);
    
    if (info.access_count < k_) {
        cold_queue_.push_back(frame_id);
        auto it = cold_queue_.end();
        --it;
        position_in_cold_[frame_id] = it;
    } else {
        size_t kth_access = GetKthAccessTime(frame_id);
        
        auto insert_pos = hot_queue_.end();
        for (auto it = hot_queue_.begin(); it != hot_queue_.end(); ++it) {
            const FrameInfo& other = frame_info_[*it];
            if (other.access_count >= k_) {
                size_t other_kth = GetKthAccessTime(*it);
                if (other_kth > kth_access) {
                    insert_pos = it;
                    break;
                }
            }
        }
        
        auto inserted_it = hot_queue_.insert(insert_pos, frame_id);
        position_in_hot_[frame_id] = inserted_it;
    }
}

void LRUKReplacer::RemoveFromQueue(size_t frame_id) {
    // Remove from cold queue if present
    auto cold_it = position_in_cold_.find(frame_id);
    if (cold_it != position_in_cold_.end()) {
        cold_queue_.erase(cold_it->second);
        position_in_cold_.erase(cold_it);
    }
    
    // Remove from hot queue if present
    auto hot_it = position_in_hot_.find(frame_id);
    if (hot_it != position_in_hot_.end()) {
        hot_queue_.erase(hot_it->second);
        position_in_hot_.erase(hot_it);
    }
}

size_t LRUKReplacer::GetKthAccessTime(size_t frame_id) const {
    const FrameInfo& info = frame_info_[frame_id];
    if (info.access_history.empty()) {
        return 0;
    }
    return info.access_history.front();
}