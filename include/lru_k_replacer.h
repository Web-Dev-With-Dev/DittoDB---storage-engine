#pragma once

#include <cstddef>
#include <deque>
#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "config.h"

/**
 * @class LRUKReplacer
 * @brief Tracks buffer pool frame access history using the LRU-K replacement algorithm.
 *
 * LRU-K addresses the sequential scan / cache pollution problem of standard LRU by
 * tracking the backward distance to the k-th previous access:
 * - Cold Queue: Frames with fewer than k accesses (evicted FIFO / oldest first).
 * - Hot Queue: Frames with k or more accesses (evicted by largest backward k-distance).
 *
 * Thread-safety is guaranteed by an internal mutex latch.
 */
class LRUKReplacer {
public:
    /**
     * @brief Construct a new LRUKReplacer instance.
     * @param num_frames Total number of frames in the buffer pool.
     * @param k Backward access distance threshold (default: 2).
     * @throws std::invalid_argument If num_frames <= 0 or k <= 0.
     */
    explicit LRUKReplacer(size_t num_frames, size_t k = 2);

    ~LRUKReplacer() = default;

    // Disable copy and move semantics
    LRUKReplacer(const LRUKReplacer &) = delete;
    LRUKReplacer &operator=(const LRUKReplacer &) = delete;
    LRUKReplacer(LRUKReplacer &&) = delete;
    LRUKReplacer &operator=(LRUKReplacer &&) = delete;

    /**
     * @brief Record that a frame was accessed at the current logical timestamp.
     * Updates the frame's access history and adjusts queue position if evictable.
     * @param frame_id Frame index being accessed.
     * @throws std::out_of_range If frame_id >= total_frames_.
     */
    void RecordAccess(frame_id_t frame_id);

    /**
     * @brief Pin a frame, marking it unevictable and removing it from replacer queues.
     * @param frame_id Frame index to pin.
     * @throws std::out_of_range If frame_id >= total_frames_.
     */
    void Pin(frame_id_t frame_id);

    /**
     * @brief Unpin a frame. If the frame has been accessed at least once, marks it
     * evictable and inserts it into the appropriate queue.
     * @param frame_id Frame index to unpin.
     * @throws std::out_of_range If frame_id >= total_frames_.
     */
    void Unpin(frame_id_t frame_id);

    /**
     * @brief Find and evict a victim frame according to LRU-K policy.
     * Checks cold queue first (FIFO), then hot queue (largest backward k-distance).
     * @return int Victim frame index (>= 0), or -1 if no evictable frame exists.
     */
    int Evict();

    /**
     * @brief Explicitly set the evictable status of a frame.
     * @param frame_id Frame index to update.
     * @param evictable true to make the frame eligible for eviction, false otherwise.
     * @throws std::out_of_range If frame_id >= total_frames_.
     */
    void SetEvictable(frame_id_t frame_id, bool evictable);

    /**
     * @brief Remove an evictable frame from the replacer and clear its access history.
     * Only evictable frames can be removed. If the frame is non-evictable or doesn't exist, this is a no-op.
     * @param frame_id Frame index to remove.
     */
    void Remove(frame_id_t frame_id);

    /**
     * @brief Get the total number of currently evictable frames across both queues.
     * @return Number of evictable frames.
     */
    size_t GetEvictableCount() const;

    /**
     * @brief Number of evictable frames (CMU 15-445 BusTub alias for GetEvictableCount()).
     * @return Number of evictable frames.
     */
    size_t Size() const { return GetEvictableCount(); }

    /**
     * @brief Get the lifetime number of frames evicted by this replacer instance.
     * @return Eviction count.
     */
    size_t GetEvictionCount() const;

    /**
     * @brief Get the total capacity (number of frames) managed by this replacer.
     * @return Total frame capacity.
     */
    size_t GetTotalFrames() const;

    /**
     * @brief Check whether a specific frame is marked as evictable.
     * @param frame_id Frame index to check.
     * @return true if evictable, false otherwise or if out of range.
     */
    bool IsEvictable(frame_id_t frame_id) const;

    /**
     * @brief Check whether a specific frame is marked as pinned.
     * @param frame_id Frame index to check.
     * @return true if pinned, false otherwise or if out of range.
     */
    bool IsPinned(frame_id_t frame_id) const;

    /**
     * @brief Get the total number of accesses recorded for a given frame.
     * @param frame_id Frame index to check.
     * @return Access count, or 0 if out of range.
     */
    size_t GetAccessCount(frame_id_t frame_id) const;

    /**
     * @brief Reset all internal queues, histories, and metrics to their initial empty state.
     */
    void Reset();

private:
    struct FrameInfo {
        frame_id_t frame_id{0};
        std::deque<size_t> access_history;  ///< Sliding window of last k timestamps
        size_t access_count{0};             ///< Total accesses recorded
        bool is_pinned{false};
        bool is_evictable{false};

        FrameInfo() = default;
    };

    size_t total_frames_;
    size_t k_;
    size_t current_time_;
    size_t eviction_count_;

    std::vector<FrameInfo> frame_info_;

    // Cold queue: frames with < k accesses (FIFO order)
    std::list<int> cold_queue_;
    std::unordered_map<int, std::list<int>::iterator> position_in_cold_;

    // Hot queue: frames with >= k accesses (sorted by k-th access timestamp)
    std::list<int> hot_queue_;
    std::unordered_map<int, std::list<int>::iterator> position_in_hot_;

    void AddToQueue(frame_id_t frame_id);
    void RemoveFromQueue(frame_id_t frame_id);
    size_t GetKthAccessTime(frame_id_t frame_id) const;

    mutable std::mutex replacer_latch_;
};