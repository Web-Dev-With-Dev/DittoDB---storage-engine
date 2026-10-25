#pragma once

#include <cstdint>
#include <cstddef>
#include <type_traits>
#include "config.h"
#include "common_page_header.h"
#include "page.h"

/**
 * @class BPlusTreeHeaderPage
 * @brief Persistent anchor page storing the root_page_id and index metadata of a B+ Tree.
 *
 * Stored inside a dedicated 4096-byte Page in BufferPoolManager (typically the first page allocated
 * for the index). Persisting root_page_id in a dedicated header page guarantees that:
 * 1. Tree growth and root splits never change the external entry point of the index.
 * 2. After crash or server restart, the engine reads this header page from disk to immediately locate the root.
 *
 * Physical Memory Layout (4096 Bytes):
 * -------------------------------------------------------------------------------------------------
 * | Byte 0 - 3        | Byte 4 - 7        | Byte 8 - 11       | Byte 12 - 15                     |
 * | PageType (4B)     | LSN (4B)          | RootPageId (4B)   | HeaderPageId (4B)                |
 * -------------------------------------------------------------------------------------------------
 * | Byte 16 - 19      | Byte 20 - 23      | Byte 24 - 31                                         |
 * | KeySize (4B)      | Depth (4B)        | RecordCount (8B)                                     |
 * -------------------------------------------------------------------------------------------------
 * | Byte 32 - 4095                                                                               |
 * | Reserved / Padding up to PAGE_SIZE (4064 Bytes)                                              |
 * -------------------------------------------------------------------------------------------------
 */
class BPlusTreeHeaderPage {
public:
    static constexpr size_t OFFSET_PAGE_TYPE = 0;
    static constexpr size_t OFFSET_LSN = 4;
    static constexpr size_t OFFSET_ROOT_PAGE_ID = 8;
    static constexpr size_t OFFSET_HEADER_PAGE_ID = 12;
    static constexpr size_t OFFSET_KEY_SIZE = 16;
    static constexpr size_t OFFSET_DEPTH = 20;
    static constexpr size_t OFFSET_RECORD_COUNT = 24;
    static constexpr size_t HEADER_METADATA_SIZE = 32;

    BPlusTreeHeaderPage() = default;

    /**
     * @brief Initialize the header page with default values.
     * @param header_page_id The physical page ID assigned to this header anchor.
     * @param root_page_id The initial root page ID (default: INVALID_PAGE_ID).
     * @param key_size Optional indexed key size.
     */
    void Init(page_id_t header_page_id, page_id_t root_page_id = INVALID_PAGE_ID, uint32_t key_size = 0) {
        page_type_ = PageType::BPLUS_TREE_HEADER_PAGE;
        lsn_ = 0;
        root_page_id_ = root_page_id;
        header_page_id_ = header_page_id;
        key_size_ = key_size;
        depth_ = (root_page_id == INVALID_PAGE_ID) ? 0 : 1;
        record_count_ = 0;
    }

    /**
     * @brief Reinterpret cast a raw Page pointer into a typed BPlusTreeHeaderPage pointer.
     * @param page Raw memory Page pointer.
     * @return BPlusTreeHeaderPage* In-place typed pointer.
     */
    static BPlusTreeHeaderPage *Cast(Page *page) {
        if (page == nullptr) {
            return nullptr;
        }
        return reinterpret_cast<BPlusTreeHeaderPage *>(page->GetData());
    }

    /**
     * @brief Reinterpret cast a const raw Page pointer into a const BPlusTreeHeaderPage pointer.
     * @param page Const raw memory Page pointer.
     * @return const BPlusTreeHeaderPage* In-place typed const pointer.
     */
    static const BPlusTreeHeaderPage *Cast(const Page *page) {
        if (page == nullptr) {
            return nullptr;
        }
        return reinterpret_cast<const BPlusTreeHeaderPage *>(page->GetData());
    }

    // ==================== Root Page ID Accessors ====================

    /**
     * @brief Get the physical page ID of the current B+ tree root.
     * @return page_id_t Root page ID, or INVALID_PAGE_ID if the tree is empty.
     */
    page_id_t GetRootPageId() const {
        return root_page_id_;
    }

    /**
     * @brief Update the root page ID (called when root splits, merges, or is initialized).
     * @param root_page_id New root page ID.
     */
    void SetRootPageId(page_id_t root_page_id) {
        root_page_id_ = root_page_id;
        if (root_page_id == INVALID_PAGE_ID) {
            depth_ = 0;
        } else if (depth_ == 0) {
            depth_ = 1;
        }
    }

    /**
     * @brief Check whether the tree currently has an active root.
     * @return true if root_page_id != INVALID_PAGE_ID, false otherwise.
     */
    bool HasRoot() const {
        return root_page_id_ != INVALID_PAGE_ID;
    }

    // ==================== Metadata Accessors ====================

    /**
     * @brief Get the header anchor page ID.
     */
    page_id_t GetHeaderPageId() const {
        return header_page_id_;
    }

    /**
     * @brief Set the header anchor page ID.
     */
    void SetHeaderPageId(page_id_t header_page_id) {
        header_page_id_ = header_page_id;
    }

    /**
     * @brief Get page type discriminator.
     */
    PageType GetPageType() const {
        return page_type_;
    }

    /**
     * @brief Get Log Sequence Number (LSN) for WAL.
     */
    lsn_t GetLSN() const {
        return lsn_;
    }

    /**
     * @brief Set Log Sequence Number (LSN) for WAL.
     */
    void SetLSN(lsn_t lsn) {
        lsn_ = lsn;
    }

    /**
     * @brief Get indexed key size.
     */
    uint32_t GetKeySize() const {
        return key_size_;
    }

    /**
     * @brief Set indexed key size.
     */
    void SetKeySize(uint32_t key_size) {
        key_size_ = key_size;
    }

    /**
     * @brief Get current depth of the tree.
     */
    uint32_t GetDepth() const {
        return depth_;
    }

    /**
     * @brief Set current depth of the tree.
     */
    void SetDepth(uint32_t depth) {
        depth_ = depth;
    }

    /**
     * @brief Get total active record count in the tree.
     */
    uint64_t GetRecordCount() const {
        return record_count_;
    }

    /**
     * @brief Set total active record count in the tree.
     */
    void SetRecordCount(uint64_t count) {
        record_count_ = count;
    }

    /**
     * @brief Increment or decrement active record count.
     */
    void IncreaseRecordCount(int64_t amount) {
        record_count_ += amount;
    }

private:
    PageType page_type_{PageType::BPLUS_TREE_HEADER_PAGE}; // 4B
    lsn_t lsn_{0};                                         // 4B
    page_id_t root_page_id_{INVALID_PAGE_ID};              // 4B
    page_id_t header_page_id_{INVALID_PAGE_ID};            // 4B
    uint32_t key_size_{0};                                 // 4B
    uint32_t depth_{0};                                    // 4B
    uint64_t record_count_{0};                             // 8B
    char reserved_[PAGE_SIZE - HEADER_METADATA_SIZE];      // 4064B padding to full page
};

static_assert(sizeof(BPlusTreeHeaderPage) == PAGE_SIZE,
              "BPlusTreeHeaderPage must exactly occupy a full 4096-byte page frame");
static_assert(std::is_standard_layout_v<BPlusTreeHeaderPage>,
              "BPlusTreeHeaderPage must have standard layout for in-place disk transmutation");
