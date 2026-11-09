#pragma once

#include <cstdint>
#include <functional>
#include <ostream>
#include <string>
#include "config.h"

/**
 * @brief Record Identifier (RID) represents the logical address of a record/tuple in a TableHeap.
 * 
 * Instead of exposing raw physical byte offsets (which change during page defragmentation,
 * compaction, or updates), tuples are uniquely addressed across the entire database by:
 *   RID = (page_id, slot_num)
 * 
 * Invariants:
 * - Decouples secondary indexes and tuple references from in-page byte offsets.
 * - Fits into a single 64-bit integer (int64_t) for efficient indexing and hashing:
 *   High 32 bits: page_id_ (cast as uint32_t)
 *   Low 32 bits:  slot_num_
 */
class RID {
private:
    page_id_t page_id_{INVALID_PAGE_ID};
    uint32_t slot_num_{0};

public:
    /** @brief Default constructor creates an invalid RID. */
    constexpr RID() = default;

    /**
     * @brief Construct an RID from page_id and slot_num.
     * @param page_id The page containing the tuple.
     * @param slot_num The slot index within the page.
     */
    constexpr RID(page_id_t page_id, uint32_t slot_num)
        : page_id_(page_id), slot_num_(slot_num) {}

    /**
     * @brief Construct an RID from a 64-bit packed integer.
     * @param rid_val The 64-bit packed integer.
     */
    constexpr explicit RID(int64_t rid_val)
        : page_id_(static_cast<page_id_t>(static_cast<uint64_t>(rid_val) >> 32)),
          slot_num_(static_cast<uint32_t>(static_cast<uint64_t>(rid_val) & 0xFFFFFFFFULL)) {}

    /**
     * @brief Convert RID to a 64-bit packed integer representation.
     * @return 64-bit scalar combining page_id and slot_num.
     */
    constexpr int64_t Get() const {
        return static_cast<int64_t>((static_cast<uint64_t>(static_cast<uint32_t>(page_id_)) << 32) |
                                    static_cast<uint64_t>(slot_num_));
    }

    /**
     * @brief Convert RID to a 64-bit packed integer representation (alias for Get()).
     * @return 64-bit scalar combining page_id and slot_num.
     */
    constexpr int64_t ToInt64() const {
        return Get();
    }

    /**
     * @brief Factory method to create an RID from a 64-bit packed integer.
     * @param rid_val 64-bit packed integer representation.
     * @return Deserialized RID instance.
     */
    static constexpr RID FromInt64(int64_t rid_val) {
        return RID(rid_val);
    }

    /**
     * @brief Get the page ID component of this RID.
     * @return Logical page ID.
     */
    constexpr page_id_t GetPageId() const { return page_id_; }

    /**
     * @brief Get the slot index component of this RID.
     * @return Slot index within the page.
     */
    constexpr uint32_t GetSlotNum() const { return slot_num_; }

    /**
     * @brief Set both page ID and slot index.
     * @param page_id Logical page ID.
     * @param slot_num Slot index within the page.
     */
    constexpr void Set(page_id_t page_id, uint32_t slot_num) {
        page_id_ = page_id;
        slot_num_ = slot_num;
    }

    /**
     * @brief Set the page ID component.
     * @param page_id Logical page ID.
     */
    constexpr void SetPageId(page_id_t page_id) {
        page_id_ = page_id;
    }

    /**
     * @brief Set the slot index component.
     * @param slot_num Slot index within the page.
     */
    constexpr void SetSlotNum(uint32_t slot_num) {
        slot_num_ = slot_num;
    }

    /**
     * @brief Check whether this RID points to a valid page.
     * @return true if page_id is valid (>= 0 and != INVALID_PAGE_ID).
     */
    constexpr bool IsValid() const {
        return page_id_ != INVALID_PAGE_ID && page_id_ >= 0;
    }

    /**
     * @brief Reset RID to default invalid state.
     */
    constexpr void Reset() {
        page_id_ = INVALID_PAGE_ID;
        slot_num_ = 0;
    }

    // Comparison operators (orders primarily by page_id, secondarily by slot_num)
    constexpr bool operator==(const RID &other) const {
        return page_id_ == other.page_id_ && slot_num_ == other.slot_num_;
    }

    constexpr bool operator!=(const RID &other) const {
        return !(*this == other);
    }

    constexpr bool operator<(const RID &other) const {
        if (page_id_ != other.page_id_) {
            return page_id_ < other.page_id_;
        }
        return slot_num_ < other.slot_num_;
    }

    constexpr bool operator<=(const RID &other) const {
        return !(other < *this);
    }

    constexpr bool operator>(const RID &other) const {
        return other < *this;
    }

    constexpr bool operator>=(const RID &other) const {
        return !(*this < other);
    }

    /**
     * @brief Format RID as human-readable string: "[page_id, slot_num]".
     * @return Formatted string.
     */
    std::string ToString() const;
};

/**
 * @brief Stream insertion operator for formatted output and logging.
 */
std::ostream &operator<<(std::ostream &os, const RID &rid);

/**
 * @brief Hash specialization allowing RID to be used as key in unordered containers.
 */
template <>
struct std::hash<RID> {
    size_t operator()(const RID &rid) const noexcept {
        return std::hash<int64_t>()(rid.Get());
    }
};
