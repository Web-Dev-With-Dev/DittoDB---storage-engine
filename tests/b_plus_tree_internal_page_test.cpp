#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <algorithm>
#include "b_plus_tree_internal_page.h"
#include "page.h"

class BPlusTreeInternalPageTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// ============================================================================
// Test 1: Binary Layout, Sizing, and Header Constants
// ============================================================================
TEST_F(BPlusTreeInternalPageTest, BinaryLayoutAndSizing) {
    using InternalPage = BPlusTreeInternalPage<int, page_id_t>;

    EXPECT_EQ(InternalPage::INTERNAL_PAGE_HEADER_SIZE, 24);
    EXPECT_LE(sizeof(InternalPage), PAGE_SIZE);

    // With 4-byte key and 4-byte page_id_t, each pair is 8 bytes.
    // (4096 - 24) / 8 = 509 entries.
    EXPECT_EQ(InternalPage::INTERNAL_PAGE_SIZE, 509);
    EXPECT_EQ(sizeof(InternalPage), 24 + 509 * sizeof(std::pair<int, page_id_t>));
    EXPECT_EQ(sizeof(InternalPage), PAGE_SIZE);
}

// ============================================================================
// Test 2: Initialization and Default State
// ============================================================================
TEST_F(BPlusTreeInternalPageTest, Initialization) {
    using InternalPage = BPlusTreeInternalPage<int, page_id_t>;
    InternalPage page;

    page.Init(42, 10, 100);

    EXPECT_EQ(page.GetPageId(), 42);
    EXPECT_EQ(page.GetParentPageId(), 10);
    EXPECT_EQ(page.GetMaxSize(), 100);
    EXPECT_EQ(page.GetSize(), 0);
    EXPECT_EQ(page.GetPageType(), PageType::BPLUS_TREE_INTERNAL_PAGE);
    EXPECT_FALSE(page.IsLeafPage());
    EXPECT_TRUE(page.IsInternalPage());
    EXPECT_FALSE(page.IsRootPage());
    EXPECT_EQ(page.GetLSN(), 0);

    // Root page initialization (parent_page_id = INVALID_PAGE_ID)
    InternalPage root_page;
    root_page.Init(1);
    EXPECT_TRUE(root_page.IsRootPage());
    EXPECT_EQ(root_page.GetParentPageId(), INVALID_PAGE_ID);
}

// ============================================================================
// Test 3: Populate and Basic Element Accessors
// ============================================================================
TEST_F(BPlusTreeInternalPageTest, PopulateAndAccessors) {
    using InternalPage = BPlusTreeInternalPage<int, page_id_t>;
    InternalPage page;
    page.Init(1, INVALID_PAGE_ID, 10);

    // Populate initial root split: children 100 and 200 separated by key 50
    page.Populate(100, 50, 200);

    EXPECT_EQ(page.GetSize(), 2);
    EXPECT_EQ(page.ValueAt(0), 100);
    EXPECT_EQ(page.KeyAt(1), 50);
    EXPECT_EQ(page.ValueAt(1), 200);

    // ValueIndex search
    EXPECT_EQ(page.ValueIndex(100), 0);
    EXPECT_EQ(page.ValueIndex(200), 1);
    EXPECT_EQ(page.ValueIndex(999), -1);

    // Direct mutators
    page.SetKeyAt(1, 60);
    page.SetValueAt(1, 250);
    EXPECT_EQ(page.KeyAt(1), 60);
    EXPECT_EQ(page.ValueAt(1), 250);
}

// ============================================================================
// Test 4: Binary-Search Child Lookup
// ============================================================================
TEST_F(BPlusTreeInternalPageTest, BinarySearchChildLookup) {
    using InternalPage = BPlusTreeInternalPage<int, page_id_t>;
    InternalPage page;
    page.Init(1, INVALID_PAGE_ID, 10);

    // Build internal page with 5 children and 4 keys:
    // Child 0 (ptr 10): keys < 20
    // Child 1 (ptr 20): 20 <= keys < 40
    // Child 2 (ptr 30): 40 <= keys < 60
    // Child 3 (ptr 40): 60 <= keys < 80
    // Child 4 (ptr 50): keys >= 80
    page.SetValueAt(0, 10);
    page.SetKeyAt(1, 20);
    page.SetValueAt(1, 20);
    page.SetKeyAt(2, 40);
    page.SetValueAt(2, 30);
    page.SetKeyAt(3, 60);
    page.SetValueAt(3, 40);
    page.SetKeyAt(4, 80);
    page.SetValueAt(4, 50);
    page.SetSize(5);

    // Keys strictly less than KeyAt(1) -> Child 0 (ptr 10)
    EXPECT_EQ(page.Lookup(5), 10);
    EXPECT_EQ(page.Lookup(19), 10);

    // Key exactly equals KeyAt(1) -> Child 1 (ptr 20)
    EXPECT_EQ(page.Lookup(20), 20);
    EXPECT_EQ(page.Lookup(25), 20);
    EXPECT_EQ(page.Lookup(39), 20);

    // Key in range [40, 60) -> Child 2 (ptr 30)
    EXPECT_EQ(page.Lookup(40), 30);
    EXPECT_EQ(page.Lookup(50), 30);
    EXPECT_EQ(page.Lookup(59), 30);

    // Key in range [60, 80) -> Child 3 (ptr 40)
    EXPECT_EQ(page.Lookup(60), 40);
    EXPECT_EQ(page.Lookup(70), 40);
    EXPECT_EQ(page.Lookup(79), 40);

    // Keys >= 80 -> Child 4 (ptr 50)
    EXPECT_EQ(page.Lookup(80), 50);
    EXPECT_EQ(page.Lookup(95), 50);
    EXPECT_EQ(page.Lookup(1000), 50);
}

// ============================================================================
// Test 5: Single Child Edge Cases in Lookup
// ============================================================================
TEST_F(BPlusTreeInternalPageTest, SingleChildAndEmptyLookup) {
    using InternalPage = BPlusTreeInternalPage<int, page_id_t>;
    InternalPage page;
    page.Init(1, INVALID_PAGE_ID, 10);

    // Empty page returns INVALID_PAGE_ID
    EXPECT_EQ(page.Lookup(100), INVALID_PAGE_ID);

    // Single child (size = 1, only ValueAt(0))
    page.SetValueAt(0, 77);
    page.SetSize(1);
    EXPECT_EQ(page.Lookup(10), 77);
    EXPECT_EQ(page.Lookup(100), 77);
    EXPECT_EQ(page.Lookup(-50), 77);
}

// ============================================================================
// Test 6: InsertNodeAfter Helper
// ============================================================================
TEST_F(BPlusTreeInternalPageTest, InsertNodeAfter) {
    using InternalPage = BPlusTreeInternalPage<int, page_id_t>;
    InternalPage page;
    page.Init(1, INVALID_PAGE_ID, 10);

    // Start with 2 children: (dummy, 10), (key=50, 20)
    page.Populate(10, 50, 20);
    ASSERT_EQ(page.GetSize(), 2);

    // 1. Insert after child 0 (ptr 10): new child 15 with separator key 25
    int new_size = page.InsertNodeAfter(10, 25, 15);
    EXPECT_EQ(new_size, 3);
    EXPECT_EQ(page.GetSize(), 3);
    EXPECT_EQ(page.ValueAt(0), 10);
    EXPECT_EQ(page.KeyAt(1), 25);
    EXPECT_EQ(page.ValueAt(1), 15);
    EXPECT_EQ(page.KeyAt(2), 50);
    EXPECT_EQ(page.ValueAt(2), 20);

    // Verify lookup routing after first insert
    EXPECT_EQ(page.Lookup(15), 10);
    EXPECT_EQ(page.Lookup(25), 15);
    EXPECT_EQ(page.Lookup(35), 15);
    EXPECT_EQ(page.Lookup(50), 20);
    EXPECT_EQ(page.Lookup(75), 20);

    // 2. Insert at the end after child 20: new child 30 with key 75
    new_size = page.InsertNodeAfter(20, 75, 30);
    EXPECT_EQ(new_size, 4);
    EXPECT_EQ(page.GetSize(), 4);
    EXPECT_EQ(page.ValueAt(3), 30);
    EXPECT_EQ(page.KeyAt(3), 75);

    // 3. Insert in the middle after child 15: new child 18 with key 40
    new_size = page.InsertNodeAfter(15, 40, 18);
    EXPECT_EQ(new_size, 5);
    EXPECT_EQ(page.GetSize(), 5);
    EXPECT_EQ(page.ValueAt(2), 18);
    EXPECT_EQ(page.KeyAt(2), 40);

    // Verify final order:
    // Pointers: 10, 15, 18, 20, 30
    // Keys:         25, 40, 50, 75
    EXPECT_EQ(page.ValueAt(0), 10);
    EXPECT_EQ(page.KeyAt(1), 25);
    EXPECT_EQ(page.ValueAt(1), 15);
    EXPECT_EQ(page.KeyAt(2), 40);
    EXPECT_EQ(page.ValueAt(2), 18);
    EXPECT_EQ(page.KeyAt(3), 50);
    EXPECT_EQ(page.ValueAt(3), 20);
    EXPECT_EQ(page.KeyAt(4), 75);
    EXPECT_EQ(page.ValueAt(4), 30);

    // 4. Insert after non-existent child returns -1
    EXPECT_EQ(page.InsertNodeAfter(999, 100, 999), -1);
    EXPECT_EQ(page.GetSize(), 5);
}

// ============================================================================
// Test 7: Remove Operation and Shift Invariants
// ============================================================================
TEST_F(BPlusTreeInternalPageTest, RemoveOperation) {
    using InternalPage = BPlusTreeInternalPage<int, page_id_t>;
    InternalPage page;
    page.Init(1, INVALID_PAGE_ID, 10);

    page.Populate(10, 20, 20);
    page.InsertNodeAfter(20, 40, 30);
    page.InsertNodeAfter(30, 60, 40);
    ASSERT_EQ(page.GetSize(), 4);

    // Remove middle entry at index 2 (key 40, ptr 30)
    page.Remove(2);
    EXPECT_EQ(page.GetSize(), 3);
    EXPECT_EQ(page.ValueAt(0), 10);
    EXPECT_EQ(page.KeyAt(1), 20);
    EXPECT_EQ(page.ValueAt(1), 20);
    EXPECT_EQ(page.KeyAt(2), 60);
    EXPECT_EQ(page.ValueAt(2), 40);

    // Lookup correctly reflects removal
    EXPECT_EQ(page.Lookup(30), 20);
    EXPECT_EQ(page.Lookup(60), 40);
}

// ============================================================================
// Test 8: Zero-Copy Raw Page Buffer Transmutation
// ============================================================================
TEST_F(BPlusTreeInternalPageTest, InPlacePageBufferTransmutation) {
    using InternalPage = BPlusTreeInternalPage<int, page_id_t>;

    Page raw_page;
    raw_page.SetPageId(99);

    InternalPage *internal = InternalPage::Cast(&raw_page);
    ASSERT_NE(internal, nullptr);

    internal->Init(99, 5, 20);
    internal->Populate(101, 500, 102);
    internal->InsertNodeAfter(102, 1000, 103);
    internal->SetLSN(777);

    // Verify mutations directly accessible on raw_page
    EXPECT_EQ(raw_page.GetPageId(), 99);
    EXPECT_EQ(raw_page.GetLSN(), 777);

    // Const cast access
    const Page &const_raw_page = raw_page;
    const InternalPage *const_internal = InternalPage::Cast(&const_raw_page);
    ASSERT_NE(const_internal, nullptr);
    EXPECT_EQ(const_internal->GetSize(), 3);
    EXPECT_EQ(const_internal->Lookup(250), 101);
    EXPECT_EQ(const_internal->Lookup(500), 102);
    EXPECT_EQ(const_internal->Lookup(1500), 103);
}

// ============================================================================
// Test 9: Custom Comparator Support
// ============================================================================
TEST_F(BPlusTreeInternalPageTest, CustomComparatorSupport) {
    // Custom descending comparator
    struct GreaterComp {
        bool operator()(int a, int b) const { return a > b; }
    };

    using DescendingInternalPage = BPlusTreeInternalPage<int, page_id_t, GreaterComp>;
    DescendingInternalPage page;
    page.Init(1, INVALID_PAGE_ID, 10);

    // In descending order: keys decrease
    // Child 0: keys > 80
    // Child 1: 80 >= keys > 50
    // Child 2: keys <= 50
    page.SetValueAt(0, 100);
    page.SetKeyAt(1, 80);
    page.SetValueAt(1, 200);
    page.SetKeyAt(2, 50);
    page.SetValueAt(2, 300);
    page.SetSize(3);

    // Descending lookup
    EXPECT_EQ(page.Lookup(90), 100);
    EXPECT_EQ(page.Lookup(80), 200);
    EXPECT_EQ(page.Lookup(60), 200);
    EXPECT_EQ(page.Lookup(50), 300);
    EXPECT_EQ(page.Lookup(10), 300);
}
