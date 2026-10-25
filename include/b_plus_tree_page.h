#pragma once

#include "common_page_header.h"
#include "page.h"

/**
 * @class BPlusTreePage
 * @brief Base class for B+Tree Internal and Leaf pages residing in a 4096-byte Page buffer.
 *
 * Inherits all metadata from CommonPageHeader:
 * - page_type (BPLUS_TREE_LEAF_PAGE or BPLUS_TREE_INTERNAL_PAGE)
 * - lsn (Log Sequence Number for WAL)
 * - size (current number of key-value pairs)
 * - max_size (maximum capacity before node split)
 * - parent_page_id (parent node pointer in tree topology)
 * - page_id (self page ID)
 */
class BPlusTreePage : public CommonPageHeader {
public:
    /**
     * @brief Reinterpret cast a raw Page pointer into a BPlusTreePage pointer.
     * @param page Raw memory Page pointer.
     * @return BPlusTreePage* In-place typed pointer.
     */
    static BPlusTreePage *Cast(Page *page) {
        if (page == nullptr) return nullptr;
        return reinterpret_cast<BPlusTreePage *>(page->GetData());
    }

    /**
     * @brief Reinterpret cast a const raw Page pointer into a const BPlusTreePage pointer.
     * @param page Const raw memory Page pointer.
     * @return const BPlusTreePage* In-place typed const pointer.
     */
    static const BPlusTreePage *Cast(const Page *page) {
        if (page == nullptr) return nullptr;
        return reinterpret_cast<const BPlusTreePage *>(page->GetData());
    }
};
