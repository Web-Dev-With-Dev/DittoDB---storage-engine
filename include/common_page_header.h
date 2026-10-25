#pragma once

#include <cstdint>
#include <cstddef>
#include <type_traits>
#include "config.h"

/**
 * @enum PageType
 * @brief Discriminator enum identifying the specialized role and memory layout of a storage page.
 */
enum class PageType : uint32_t {
    INVALID_PAGE = 0,
    BPLUS_TREE_LEAF_PAGE = 1,
    BPLUS_TREE_INTERNAL_PAGE = 2,
    TABLE_PAGE = 3,
    HASH_TABLE_DIRECTORY_PAGE = 4,
    HASH_TABLE_BUCKET_PAGE = 5
};

/**
 * @class CommonPageHeader
 * @brief Canonical 24-byte common page header placed at offset 0 of every structured storage page.
 *
 * Exact Physical Memory Layout (24 Bytes):
 * ---------------------------------------------------------------------------------------------------------
 * | Byte 0 - 3        | Byte 4 - 7    | Byte 8 - 11       | Byte 12 - 15    | Byte 16 - 19      | Byte 20 - 23    |
 * ---------------------------------------------------------------------------------------------------------
 * | PageType (4B)     | LSN (4B)      | Current Size (4B) | Max Size (4B)   | Parent PageId(4B) | PageId (4B)     |
 * ---------------------------------------------------------------------------------------------------------
 * | <----------------------------- 4072 Bytes Payload / Subclass Data ----------------------------------> |
 * ---------------------------------------------------------------------------------------------------------
 *
 * Invariants & Design Principles:
 * 1. Zero VTable Overhead: No virtual functions; guaranteed to be standard-layout / trivially copyable.
 * 2. Raw In-Place Transmutation: Directly castable to/from raw 4096-byte Page buffers (`Page::GetData()`).
 * 3. 4-Byte Natural Word Alignment: Every field is a 32-bit scalar, preventing unaligned memory access penalties.
 * 4. WAL Protocol Ready: Dedicated LSN field enforces the Write-Ahead Logging Invariant during buffer pool flush.
 */
class CommonPageHeader {
public:
    static constexpr size_t OFFSET_PAGE_TYPE = 0;
    static constexpr size_t OFFSET_LSN = 4;
    static constexpr size_t OFFSET_CURRENT_SIZE = 8;
    static constexpr size_t OFFSET_MAX_SIZE = 12;
    static constexpr size_t OFFSET_PARENT_PAGE_ID = 16;
    static constexpr size_t OFFSET_PAGE_ID = 20;
    static constexpr size_t COMMON_PAGE_HEADER_SIZE = 24;

    CommonPageHeader() = default;

    // ==================== Page Type Operations ====================
    PageType GetPageType() const { return page_type_; }
    void SetPageType(PageType type) { page_type_ = type; }
    bool IsLeafPage() const { return page_type_ == PageType::BPLUS_TREE_LEAF_PAGE; }
    bool IsInternalPage() const { return page_type_ == PageType::BPLUS_TREE_INTERNAL_PAGE; }

    // ==================== Size & Capacity Tracking ====================
    int GetSize() const { return size_; }
    void SetSize(int size) { size_ = size; }
    void IncreaseSize(int amount) { size_ += amount; }
    int GetMaxSize() const { return max_size_; }
    void SetMaxSize(int max_size) { max_size_ = max_size; }
    int GetMinSize() const { return (max_size_ + 1) / 2; }

    // ==================== Tree & Topology Navigation ====================
    page_id_t GetParentPageId() const { return parent_page_id_; }
    void SetParentPageId(page_id_t parent_page_id) { parent_page_id_ = parent_page_id; }
    bool IsRootPage() const { return parent_page_id_ == INVALID_PAGE_ID; }

    // ==================== Page Identification ====================
    page_id_t GetPageId() const { return page_id_; }
    void SetPageId(page_id_t page_id) { page_id_ = page_id; }

    // ==================== WAL & Recovery Metadata ====================
    lsn_t GetLSN() const { return lsn_; }
    void SetLSN(lsn_t lsn) { lsn_ = lsn; }

private:
    PageType page_type_{PageType::INVALID_PAGE}; // 4 bytes
    lsn_t lsn_{INVALID_LSN};                     // 4 bytes (WAL Log Sequence Number)
    int32_t size_{0};                            // 4 bytes (Current count of elements/records)
    int32_t max_size_{0};                        // 4 bytes (Max capacity before split)
    page_id_t parent_page_id_{INVALID_PAGE_ID};  // 4 bytes (Parent page ID in tree topology)
    page_id_t page_id_{INVALID_PAGE_ID};         // 4 bytes (Self page ID for disk verification)
};

static_assert(sizeof(CommonPageHeader) == 24, "CommonPageHeader must be exactly 24 bytes in size");
static_assert(std::is_standard_layout_v<CommonPageHeader>, "CommonPageHeader must satisfy standard layout for direct disk casting");
