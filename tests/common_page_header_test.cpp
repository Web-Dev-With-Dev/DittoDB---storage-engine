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

// ============================================================================
// Test 5: Page Integration - Accessors, LSN, and PageType on Page Class
// ============================================================================
TEST_F(CommonPageHeaderTest, PageClassIntegration) {
    Page page;

    // Verify initial header state on Page construction
    EXPECT_EQ(page.GetPageId(), INVALID_PAGE_ID);
    EXPECT_EQ(page.GetPageType(), PageType::INVALID_PAGE);
    EXPECT_EQ(page.GetLSN(), 0);
    EXPECT_NE(page.GetHeader(), nullptr);

    // Mutate through Page methods
    page.SetPageId(50);
    page.SetPageType(PageType::BPLUS_TREE_INTERNAL_PAGE);
    page.SetLSN(98765);
    page.GetHeader()->SetPageId(50);

    EXPECT_EQ(page.GetPageId(), 50);
    EXPECT_EQ(page.GetPageType(), PageType::BPLUS_TREE_INTERNAL_PAGE);
    EXPECT_EQ(page.GetLSN(), 98765);

    // Verify synchronization with underlying CommonPageHeader
    EXPECT_EQ(page.GetHeader()->GetPageId(), 50);
    EXPECT_EQ(page.GetHeader()->GetPageType(), PageType::BPLUS_TREE_INTERNAL_PAGE);
    EXPECT_EQ(page.GetHeader()->GetLSN(), 98765);

    // Test ResetMemory() clears page data
    page.ResetMemory();
    EXPECT_EQ(page.GetLSN(), 0);
    EXPECT_EQ(page.GetPageId(), 50); // Preserves cached page_id tag
}

// ============================================================================
// Test 6: BPlusTreePage Zero-Copy In-Place Casting
// ============================================================================
#include "b_plus_tree_page.h"

TEST_F(CommonPageHeaderTest, BPlusTreePageCasting) {
    Page page;
    page.SetPageId(12);

    BPlusTreePage *b_page = BPlusTreePage::Cast(&page);
    ASSERT_NE(b_page, nullptr);

    b_page->SetPageType(PageType::BPLUS_TREE_LEAF_PAGE);
    b_page->SetSize(16);
    b_page->SetMaxSize(32);
    b_page->SetParentPageId(5);
    b_page->SetLSN(1001);
    b_page->SetPageId(12);

    EXPECT_TRUE(b_page->IsLeafPage());
    EXPECT_FALSE(b_page->IsInternalPage());
    EXPECT_FALSE(b_page->IsRootPage());
    EXPECT_EQ(b_page->GetSize(), 16);
    EXPECT_EQ(b_page->GetMaxSize(), 32);
    EXPECT_EQ(b_page->GetMinSize(), 16);
    EXPECT_EQ(b_page->GetParentPageId(), 5);
    EXPECT_EQ(b_page->GetPageId(), 12);
    EXPECT_EQ(b_page->GetLSN(), 1001);

    // Const cast overload
    const Page &c_page = page;
    const BPlusTreePage *cb_page = BPlusTreePage::Cast(&c_page);
    ASSERT_NE(cb_page, nullptr);
    EXPECT_EQ(cb_page->GetPageId(), 12);
    EXPECT_EQ(cb_page->GetSize(), 16);
}

// ============================================================================
// Test 7: TablePage Integration with Common Header
// ============================================================================
#include "table_page.h"

TEST_F(CommonPageHeaderTest, TablePageIntegration) {
    TablePage tp;
    tp.Init(88);

    EXPECT_EQ(tp.GetTablePageId(), 88);
    EXPECT_EQ(tp.GetPageId(), 88);
    // TablePage shares OFFSET_LSN = 4 with CommonPageHeader
    tp.SetLSN(12345);
    EXPECT_EQ(tp.GetLSN(), 12345);
    EXPECT_EQ(tp.Page::GetLSN(), 12345);
}

