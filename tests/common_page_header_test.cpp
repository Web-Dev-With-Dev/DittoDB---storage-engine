#include <gtest/gtest.h>
#include <cstring>
#include "common_page_header.h"
#include "page.h"

class CommonPageHeaderTest : public ::testing::Test {};

// ============================================================================
// Test 1: Binary Layout, Alignment, and Size Invariants
// ============================================================================
TEST_F(CommonPageHeaderTest, BinaryLayoutAndSize) {
    // Header must be exactly 24 bytes (4B type + 4B lsn + 4B size + 4B max_size + 4B parent + 4B page_id)
    EXPECT_EQ(sizeof(CommonPageHeader), 24);
    EXPECT_EQ(CommonPageHeader::COMMON_PAGE_HEADER_SIZE, 24);

    // Must be 4-byte aligned (all members are 32-bit words)
    EXPECT_EQ(alignof(CommonPageHeader), 4);

    // Standard layout and trivially copyable guarantees safe disk serialization
    EXPECT_TRUE(std::is_standard_layout_v<CommonPageHeader>);
    EXPECT_TRUE(std::is_trivially_copyable_v<CommonPageHeader>);

    // Verify remaining space on a 4096-byte database page
    EXPECT_EQ(PAGE_SIZE - sizeof(CommonPageHeader), 4072);
}

// ============================================================================
// Test 2: Default Construction & Sentinel Values
// ============================================================================
TEST_F(CommonPageHeaderTest, DefaultConstruction) {
    CommonPageHeader header;

    EXPECT_EQ(header.GetPageType(), PageType::INVALID_PAGE);
    EXPECT_EQ(header.GetLSN(), INVALID_LSN);
    EXPECT_EQ(header.GetSize(), 0);
    EXPECT_EQ(header.GetMaxSize(), 0);
    EXPECT_EQ(header.GetParentPageId(), INVALID_PAGE_ID);
    EXPECT_EQ(header.GetPageId(), INVALID_PAGE_ID);

    // A default-initialized node with INVALID_PAGE_ID parent is treated as root
    EXPECT_TRUE(header.IsRootPage());
    EXPECT_FALSE(header.IsLeafPage());
    EXPECT_FALSE(header.IsInternalPage());
}

// ============================================================================
// Test 3: Getters, Setters, and Mutators
// ============================================================================
TEST_F(CommonPageHeaderTest, GettersAndSetters) {
    CommonPageHeader header;

    // Page Type
    header.SetPageType(PageType::BPLUS_TREE_LEAF_PAGE);
    EXPECT_EQ(header.GetPageType(), PageType::BPLUS_TREE_LEAF_PAGE);
    EXPECT_TRUE(header.IsLeafPage());
    EXPECT_FALSE(header.IsInternalPage());

    header.SetPageType(PageType::BPLUS_TREE_INTERNAL_PAGE);
    EXPECT_EQ(header.GetPageType(), PageType::BPLUS_TREE_INTERNAL_PAGE);
    EXPECT_FALSE(header.IsLeafPage());
    EXPECT_TRUE(header.IsInternalPage());

    // Page Identification
    header.SetPageId(42);
    EXPECT_EQ(header.GetPageId(), 42);

    header.SetParentPageId(10);
    EXPECT_EQ(header.GetParentPageId(), 10);
    EXPECT_FALSE(header.IsRootPage());

    header.SetParentPageId(INVALID_PAGE_ID);
    EXPECT_TRUE(header.IsRootPage());

    // Size & Capacity
    header.SetMaxSize(100);
    EXPECT_EQ(header.GetMaxSize(), 100);
    EXPECT_EQ(header.GetMinSize(), 50); // (100 + 1) / 2 = 50

    header.SetMaxSize(101);
    EXPECT_EQ(header.GetMinSize(), 51); // (101 + 1) / 2 = 51

    header.SetSize(10);
    EXPECT_EQ(header.GetSize(), 10);
    header.IncreaseSize(5);
    EXPECT_EQ(header.GetSize(), 15);
    header.IncreaseSize(-3);
    EXPECT_EQ(header.GetSize(), 12);

    // WAL LSN (Log Sequence Number)
    header.SetLSN(1048576);
    EXPECT_EQ(header.GetLSN(), 1048576);
}

// ============================================================================
// Test 4: In-Place Raw Page Memory Casting & Byte-Level Offsets
// ============================================================================
TEST_F(CommonPageHeaderTest, InPlaceRawPageTransmutation) {
    Page page;
    char *raw_data = page.GetData();
    std::memset(raw_data, 0, PAGE_SIZE);

    // Transmute raw page memory into CommonPageHeader
    auto *header = reinterpret_cast<CommonPageHeader *>(raw_data);

    header->SetPageType(PageType::BPLUS_TREE_LEAF_PAGE); // 1
    header->SetLSN(5555);
    header->SetSize(25);
    header->SetMaxSize(50);
    header->SetParentPageId(7);
    header->SetPageId(99);

    // Verify fields by directly reading raw 32-bit integers at exact byte offsets
    uint32_t raw_type = *reinterpret_cast<uint32_t *>(raw_data + CommonPageHeader::OFFSET_PAGE_TYPE);
    int32_t raw_lsn = *reinterpret_cast<int32_t *>(raw_data + CommonPageHeader::OFFSET_LSN);
    int32_t raw_size = *reinterpret_cast<int32_t *>(raw_data + CommonPageHeader::OFFSET_CURRENT_SIZE);
    int32_t raw_max_size = *reinterpret_cast<int32_t *>(raw_data + CommonPageHeader::OFFSET_MAX_SIZE);
    page_id_t raw_parent = *reinterpret_cast<page_id_t *>(raw_data + CommonPageHeader::OFFSET_PARENT_PAGE_ID);
    page_id_t raw_page_id = *reinterpret_cast<page_id_t *>(raw_data + CommonPageHeader::OFFSET_PAGE_ID);

    EXPECT_EQ(raw_type, static_cast<uint32_t>(PageType::BPLUS_TREE_LEAF_PAGE));
    EXPECT_EQ(raw_lsn, 5555);
    EXPECT_EQ(raw_size, 25);
    EXPECT_EQ(raw_max_size, 50);
    EXPECT_EQ(raw_parent, 7);
    EXPECT_EQ(raw_page_id, 99);

    // Verify payload space immediately following the 24-byte header is untouched
    for (size_t i = 24; i < PAGE_SIZE; ++i) {
        EXPECT_EQ(raw_data[i], 0);
    }
}
