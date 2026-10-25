#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <algorithm>
#include "b_plus_tree_leaf_page.h"
#include "page.h"
#include "rid.h"

class BPlusTreeLeafPageTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// ============================================================================
// Test 1: Sizing, Binary Layout, and Physical Alignment
// ============================================================================
TEST_F(BPlusTreeLeafPageTest, BinaryLayoutAndSizing) {
    using LeafPage = BPlusTreeLeafPage<int, RID>;

    EXPECT_LE(sizeof(LeafPage), PAGE_SIZE);
    EXPECT_GT(LeafPage::LEAF_PAGE_SIZE, 0);

    // With int (4B) and RID (8B, 4B aligned), MappingType is 12 bytes.
    // (4096 - 32) / 12 = 338 entries.
    EXPECT_EQ(sizeof(std::pair<int, RID>), 12);
    EXPECT_EQ(LeafPage::LEAF_PAGE_SIZE, 338);
    EXPECT_LE(sizeof(LeafPage), 4096);
}

// ============================================================================
// Test 2: Initialization and Default State
// ============================================================================
TEST_F(BPlusTreeLeafPageTest, Initialization) {
    using LeafPage = BPlusTreeLeafPage<int, RID>;
    LeafPage page;

    page.Init(101, 10, 50);

    EXPECT_EQ(page.GetPageId(), 101);
    EXPECT_EQ(page.GetParentPageId(), 10);
    EXPECT_EQ(page.GetMaxSize(), 50);
    EXPECT_EQ(page.GetSize(), 0);
    EXPECT_EQ(page.GetPageType(), PageType::BPLUS_TREE_LEAF_PAGE);
    EXPECT_TRUE(page.IsLeafPage());
    EXPECT_FALSE(page.IsInternalPage());
    EXPECT_EQ(page.GetNextPageId(), INVALID_PAGE_ID);
    EXPECT_EQ(page.GetLSN(), 0);
}

// ============================================================================
// Test 3: Next Page Pointer and Sequential Linking
// ============================================================================
TEST_F(BPlusTreeLeafPageTest, NextPagePointerLinking) {
    using LeafPage = BPlusTreeLeafPage<int, RID>;
    LeafPage leaf1;
    LeafPage leaf2;
    LeafPage leaf3;

    leaf1.Init(1);
    leaf2.Init(2);
    leaf3.Init(3);

    leaf1.SetNextPageId(2);
    leaf2.SetNextPageId(3);
    leaf3.SetNextPageId(INVALID_PAGE_ID);

    EXPECT_EQ(leaf1.GetNextPageId(), 2);
    EXPECT_EQ(leaf2.GetNextPageId(), 3);
    EXPECT_EQ(leaf3.GetNextPageId(), INVALID_PAGE_ID);
}

// ============================================================================
// Test 4: Sorted Insert and Automatic Order Maintenance
// ============================================================================
TEST_F(BPlusTreeLeafPageTest, SortedInsert) {
    using LeafPage = BPlusTreeLeafPage<int, RID>;
    LeafPage page;
    page.Init(1, INVALID_PAGE_ID, 10);

    // Insert keys in non-sorted, arbitrary order
    std::vector<int> keys = {50, 20, 80, 10, 40, 70, 90, 30, 60};
    for (size_t i = 0; i < keys.size(); ++i) {
        int key = keys[i];
        RID rid(1, static_cast<uint32_t>(key));
        int new_size = page.Insert(key, rid);
        EXPECT_EQ(new_size, static_cast<int>(i + 1));
    }

    EXPECT_EQ(page.GetSize(), 9);

    // Verify all keys are in strictly ascending sorted order
    std::vector<int> expected_keys = {10, 20, 30, 40, 50, 60, 70, 80, 90};
    for (int i = 0; i < page.GetSize(); ++i) {
        EXPECT_EQ(page.KeyAt(i), expected_keys[i]);
        EXPECT_EQ(page.ValueAt(i), RID(1, static_cast<uint32_t>(expected_keys[i])));
        EXPECT_EQ(page.ItemAt(i).first, expected_keys[i]);
        EXPECT_EQ(page.ItemAt(i).second, RID(1, static_cast<uint32_t>(expected_keys[i])));
    }

    // Verify duplicate key insertion is rejected and returns current size
    int size_after_dup = page.Insert(50, RID(1, 999));
    EXPECT_EQ(size_after_dup, 9);
    EXPECT_EQ(page.GetSize(), 9);
    EXPECT_EQ(page.ValueAt(4), RID(1, 50)); // Original RID preserved
}

// ============================================================================
// Test 5: Binary-Search Lookup
// ============================================================================
TEST_F(BPlusTreeLeafPageTest, BinarySearchLookup) {
    using LeafPage = BPlusTreeLeafPage<int, RID>;
    LeafPage page;
    page.Init(1, INVALID_PAGE_ID, 10);

    // Empty page lookup
    RID result;
    EXPECT_FALSE(page.Lookup(10, &result));

    // Populate keys: 10, 20, 30, 40, 50
    for (int k : {10, 20, 30, 40, 50}) {
        page.Insert(k, RID(5, static_cast<uint32_t>(k * 10)));
    }

    // Existing keys
    for (int k : {10, 20, 30, 40, 50}) {
        RID found_rid;
        EXPECT_TRUE(page.Lookup(k, &found_rid));
        EXPECT_EQ(found_rid, RID(5, static_cast<uint32_t>(k * 10)));
    }

    // Non-existent keys: smaller, middle, larger
    EXPECT_FALSE(page.Lookup(5, &result));
    EXPECT_FALSE(page.Lookup(25, &result));
    EXPECT_FALSE(page.Lookup(55, &result));
    EXPECT_FALSE(page.Lookup(-100, &result));
    EXPECT_FALSE(page.Lookup(999, &result));

    // KeyIndex check
    EXPECT_EQ(page.KeyIndex(30), 2);
    EXPECT_EQ(page.KeyIndex(99), -1);
}

// ============================================================================
// Test 6: Record Removal
// ============================================================================
TEST_F(BPlusTreeLeafPageTest, RemoveAndDeleteRecord) {
    using LeafPage = BPlusTreeLeafPage<int, RID>;
    LeafPage page;
    page.Init(1, INVALID_PAGE_ID, 10);

    for (int k : {10, 20, 30, 40, 50}) {
        page.Insert(k, RID(1, static_cast<uint32_t>(k)));
    }
    ASSERT_EQ(page.GetSize(), 5);

    // 1. Delete middle record (30)
    EXPECT_EQ(page.RemoveAndDeleteRecord(30), 4);
    EXPECT_EQ(page.GetSize(), 4);
    EXPECT_FALSE(page.Lookup(30));
    EXPECT_EQ(page.KeyAt(0), 10);
    EXPECT_EQ(page.KeyAt(1), 20);
    EXPECT_EQ(page.KeyAt(2), 40);
    EXPECT_EQ(page.KeyAt(3), 50);

    // 2. Delete first record (10)
    EXPECT_EQ(page.RemoveAndDeleteRecord(10), 3);
    EXPECT_EQ(page.GetSize(), 3);
    EXPECT_FALSE(page.Lookup(10));
    EXPECT_EQ(page.KeyAt(0), 20);

    // 3. Delete last record (50)
    EXPECT_EQ(page.RemoveAndDeleteRecord(50), 2);
    EXPECT_EQ(page.GetSize(), 2);
    EXPECT_FALSE(page.Lookup(50));
    EXPECT_EQ(page.KeyAt(1), 40);

    // 4. Delete non-existent record returns current size
    EXPECT_EQ(page.RemoveAndDeleteRecord(999), 2);
    EXPECT_EQ(page.GetSize(), 2);
}

// ============================================================================
// Test 7: MoveHalfTo (Node Split Helper)
// ============================================================================
TEST_F(BPlusTreeLeafPageTest, MoveHalfToSplit) {
    using LeafPage = BPlusTreeLeafPage<int, RID>;
    LeafPage left_leaf;
    LeafPage right_leaf;

    left_leaf.Init(1, INVALID_PAGE_ID, 10);
    right_leaf.Init(2, INVALID_PAGE_ID, 10);
    left_leaf.SetNextPageId(99);

    // Insert 6 elements: 10, 20, 30, 40, 50, 60
    for (int k : {10, 20, 30, 40, 50, 60}) {
        left_leaf.Insert(k, RID(1, static_cast<uint32_t>(k)));
    }

    // Split node: upper half moves to right_leaf
    left_leaf.MoveHalfTo(&right_leaf);

    // Left should retain first 3 entries (10, 20, 30)
    EXPECT_EQ(left_leaf.GetSize(), 3);
    EXPECT_EQ(left_leaf.KeyAt(0), 10);
    EXPECT_EQ(left_leaf.KeyAt(1), 20);
    EXPECT_EQ(left_leaf.KeyAt(2), 30);

    // Right should receive last 3 entries (40, 50, 60)
    EXPECT_EQ(right_leaf.GetSize(), 3);
    EXPECT_EQ(right_leaf.KeyAt(0), 40);
    EXPECT_EQ(right_leaf.KeyAt(1), 50);
    EXPECT_EQ(right_leaf.KeyAt(2), 60);

    // Sequential chain must be updated: left_leaf -> right_leaf -> 99
    EXPECT_EQ(left_leaf.GetNextPageId(), 2);
    EXPECT_EQ(right_leaf.GetNextPageId(), 99);
}

// ============================================================================
// Test 8: MoveAllTo (Node Merge Helper)
// ============================================================================
TEST_F(BPlusTreeLeafPageTest, MoveAllToMerge) {
    using LeafPage = BPlusTreeLeafPage<int, RID>;
    LeafPage left_leaf;
    LeafPage right_leaf;

    left_leaf.Init(1, INVALID_PAGE_ID, 10);
    right_leaf.Init(2, INVALID_PAGE_ID, 10);

    left_leaf.SetNextPageId(2);
    right_leaf.SetNextPageId(3);

    for (int k : {10, 20}) {
        left_leaf.Insert(k, RID(1, static_cast<uint32_t>(k)));
    }
    for (int k : {30, 40}) {
        right_leaf.Insert(k, RID(2, static_cast<uint32_t>(k)));
    }

    // Merge right_leaf into left_leaf
    right_leaf.MoveAllTo(&left_leaf);

    EXPECT_EQ(left_leaf.GetSize(), 4);
    EXPECT_EQ(left_leaf.KeyAt(0), 10);
    EXPECT_EQ(left_leaf.KeyAt(1), 20);
    EXPECT_EQ(left_leaf.KeyAt(2), 30);
    EXPECT_EQ(left_leaf.KeyAt(3), 40);

    // Linked chain updated
    EXPECT_EQ(left_leaf.GetNextPageId(), 3);
    EXPECT_EQ(right_leaf.GetSize(), 0);
    EXPECT_EQ(right_leaf.GetNextPageId(), INVALID_PAGE_ID);
}

// ============================================================================
// Test 9: In-Place Raw Page Buffer Transmutation
// ============================================================================
TEST_F(BPlusTreeLeafPageTest, InPlacePageBufferTransmutation) {
    using LeafPage = BPlusTreeLeafPage<int, RID>;

    Page raw_page;
    raw_page.SetPageId(55);

    LeafPage *leaf = LeafPage::Cast(&raw_page);
    ASSERT_NE(leaf, nullptr);

    leaf->Init(55, 1, 20);
    leaf->SetNextPageId(56);
    leaf->Insert(100, RID(55, 1));
    leaf->Insert(200, RID(55, 2));
    leaf->SetLSN(888);

    // Check through raw_page API
    EXPECT_EQ(raw_page.GetPageId(), 55);
    EXPECT_EQ(raw_page.GetLSN(), 888);

    // Check through const cast
    const Page &const_page = raw_page;
    const LeafPage *const_leaf = LeafPage::Cast(&const_page);
    ASSERT_NE(const_leaf, nullptr);
    EXPECT_EQ(const_leaf->GetSize(), 2);
    EXPECT_EQ(const_leaf->GetNextPageId(), 56);

    RID found_rid;
    EXPECT_TRUE(const_leaf->Lookup(100, &found_rid));
    EXPECT_EQ(found_rid, RID(55, 1));
}

// ============================================================================
// Test 10: Custom Comparator Support
// ============================================================================
TEST_F(BPlusTreeLeafPageTest, CustomComparatorSupport) {
    struct GreaterComp {
        bool operator()(int a, int b) const { return a > b; }
    };

    using DescendingLeafPage = BPlusTreeLeafPage<int, RID, GreaterComp>;
    DescendingLeafPage page;
    page.Init(1, INVALID_PAGE_ID, 10);

    page.Insert(30, RID(1, 30));
    page.Insert(90, RID(1, 90));
    page.Insert(10, RID(1, 10));
    page.Insert(60, RID(1, 60));

    EXPECT_EQ(page.GetSize(), 4);
    // Keys in descending order: 90, 60, 30, 10
    EXPECT_EQ(page.KeyAt(0), 90);
    EXPECT_EQ(page.KeyAt(1), 60);
    EXPECT_EQ(page.KeyAt(2), 30);
    EXPECT_EQ(page.KeyAt(3), 10);

    RID r;
    EXPECT_TRUE(page.Lookup(60, &r));
    EXPECT_EQ(r, RID(1, 60));
    EXPECT_FALSE(page.Lookup(50));
}
