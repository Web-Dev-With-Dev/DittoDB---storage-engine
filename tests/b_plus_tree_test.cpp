#include <gtest/gtest.h>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <vector>
#include <algorithm>

#include "b_plus_tree.h"
#include "b_plus_tree_header_page.h"
#include "b_plus_tree_internal_page.h"
#include "b_plus_tree_leaf_page.h"
#include "buffer_pool_manager.h"
#include "disk_manager.h"
#include "generic_key.h"
#include "rid.h"

namespace fs = std::filesystem;

class BPlusTreeTest : public ::testing::Test {
protected:
    const std::string db_file_ = "test_b_plus_tree.db";
    std::unique_ptr<DiskManager> disk_manager_;
    std::unique_ptr<BufferPoolManager> bpm_;

    void SetUp() override {
        if (fs::exists(db_file_)) {
            fs::remove(db_file_);
        }
        disk_manager_ = std::make_unique<DiskManager>(db_file_);
        bpm_ = std::make_unique<BufferPoolManager>(50, disk_manager_.get());
    }

    void TearDown() override {
        bpm_.reset();
        disk_manager_.reset();
        if (fs::exists(db_file_)) {
            fs::remove(db_file_);
        }
    }

    // Helper to assert that all frames in the buffer pool are unpinned (pin count == 0)
    void AssertAllPagesUnpinned() {
        for (size_t i = 0; i < bpm_->GetPoolSize(); ++i) {
            Page *page = bpm_->GetPage(static_cast<frame_id_t>(i));
            if (page != nullptr) {
                EXPECT_EQ(page->GetPinCount(), 0)
                    << "Frame " << i << " (page_id " << page->GetPageId() << ") has leaked pin count!";
            }
        }
    }
};

// ============================================================================
// Test 1: Empty Tree GetValue Returns False and Never Leaks Pins
// ============================================================================
TEST_F(BPlusTreeTest, EmptyTreeGetValue) {
    BPlusTree<int, RID> tree("empty_tree", bpm_.get());

    EXPECT_TRUE(tree.IsEmpty());
    EXPECT_EQ(tree.GetRootPageId(), INVALID_PAGE_ID);

    RID val;
    EXPECT_FALSE(tree.GetValue(42, &val));
    EXPECT_FALSE(tree.GetValue(0, &val));
    EXPECT_FALSE(tree.GetValue(-1, &val));

    std::vector<RID> vec_val;
    EXPECT_FALSE(tree.GetValue(42, &vec_val));
    EXPECT_TRUE(vec_val.empty());

    // Header page should be unpinned
    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 2: Single Leaf Tree Point Lookup (Binary Search on Leaf)
// ============================================================================
TEST_F(BPlusTreeTest, SingleLeafPointLookup) {
    BPlusTree<int, RID> tree("single_leaf", bpm_.get(), std::less<int>{}, 10, 10);

    // Insert keys: 10, 20, 30, 40, 50
    EXPECT_TRUE(tree.Insert(30, RID(1, 3)));
    EXPECT_TRUE(tree.Insert(10, RID(1, 1)));
    EXPECT_TRUE(tree.Insert(50, RID(1, 5)));
    EXPECT_TRUE(tree.Insert(20, RID(1, 2)));
    EXPECT_TRUE(tree.Insert(40, RID(1, 4)));

    EXPECT_FALSE(tree.IsEmpty());
    EXPECT_EQ(tree.GetDepth(), 1u);
    EXPECT_EQ(tree.GetRecordCount(), 5u);

    // Verify all pages are unpinned after inserts
    AssertAllPagesUnpinned();

    // Positive lookups via binary search
    RID result;
    EXPECT_TRUE(tree.GetValue(10, &result));
    EXPECT_EQ(result, RID(1, 1));
    AssertAllPagesUnpinned();

    EXPECT_TRUE(tree.GetValue(20, &result));
    EXPECT_EQ(result, RID(1, 2));
    AssertAllPagesUnpinned();

    EXPECT_TRUE(tree.GetValue(30, &result));
    EXPECT_EQ(result, RID(1, 3));
    AssertAllPagesUnpinned();

    EXPECT_TRUE(tree.GetValue(40, &result));
    EXPECT_EQ(result, RID(1, 4));
    AssertAllPagesUnpinned();

    EXPECT_TRUE(tree.GetValue(50, &result));
    EXPECT_EQ(result, RID(1, 5));
    AssertAllPagesUnpinned();

    // Negative lookups (keys below min, between keys, above max)
    EXPECT_FALSE(tree.GetValue(5, &result));
    AssertAllPagesUnpinned();

    EXPECT_FALSE(tree.GetValue(25, &result));
    AssertAllPagesUnpinned();

    EXPECT_FALSE(tree.GetValue(99, &result));
    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 3: Insert into Empty Tree (Root Leaf Allocation & Metadata Initialization)
// ============================================================================
TEST_F(BPlusTreeTest, InsertIntoEmptyTree) {
    BPlusTree<int, RID> tree("empty_insert_tree", bpm_.get(), std::less<int>{}, 10, 10);

    // Initial state
    EXPECT_TRUE(tree.IsEmpty());
    EXPECT_EQ(tree.GetRootPageId(), INVALID_PAGE_ID);
    EXPECT_EQ(tree.GetDepth(), 0u);
    EXPECT_EQ(tree.GetRecordCount(), 0u);

    // First insert into empty tree
    bool success = tree.Insert(42, RID(10, 42));
    EXPECT_TRUE(success);

    // State after insertion
    EXPECT_FALSE(tree.IsEmpty());
    EXPECT_NE(tree.GetRootPageId(), INVALID_PAGE_ID);
    EXPECT_EQ(tree.GetDepth(), 1u);
    EXPECT_EQ(tree.GetRecordCount(), 1u);

    // Verify root is a leaf page
    Page *root_page = bpm_->FetchPage(tree.GetRootPageId());
    ASSERT_NE(root_page, nullptr);
    auto *leaf = BPlusTreeLeafPage<int, RID>::Cast(root_page);
    EXPECT_TRUE(leaf->IsLeafPage());
    EXPECT_EQ(leaf->GetSize(), 1);
    EXPECT_EQ(leaf->KeyAt(0), 42);
    EXPECT_EQ(leaf->ValueAt(0), RID(10, 42));
    EXPECT_EQ(leaf->GetParentPageId(), INVALID_PAGE_ID);
    bpm_->UnpinPage(tree.GetRootPageId(), false);

    // Value lookup
    RID found;
    EXPECT_TRUE(tree.GetValue(42, &found));
    EXPECT_EQ(found, RID(10, 42));

    AssertAllPagesUnpinned();

    // Direct invocation on already non-empty tree should return false
    EXPECT_FALSE(tree.InsertIntoEmptyTree(99, RID(10, 99)));
    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 4: Insert for a Leaf With Space (Maintaining Sorted Order)
// ============================================================================
TEST_F(BPlusTreeTest, InsertLeafWithSpaceSortedOrder) {
    // Leaf capacity 10, internal capacity 10
    BPlusTree<int, RID> tree("leaf_with_space_tree", bpm_.get(), std::less<int>{}, 10, 10);

    // Insert keys in non-sorted, arbitrary order
    std::vector<int> insert_keys = {50, 20, 80, 10, 40, 70, 30, 60};
    for (size_t i = 0; i < insert_keys.size(); ++i) {
        int key = insert_keys[i];
        bool ok = tree.Insert(key, RID(1, static_cast<uint32_t>(key)));
        EXPECT_TRUE(ok);
        EXPECT_EQ(tree.GetRecordCount(), i + 1);
        AssertAllPagesUnpinned();
    }

    // Verify tree has not split (single leaf root)
    EXPECT_EQ(tree.GetDepth(), 1u);
    EXPECT_EQ(tree.GetRecordCount(), 8u);

    // Inspect underlying LeafPage array to verify strict ascending sort order
    Page *root_frame = bpm_->FetchPage(tree.GetRootPageId());
    ASSERT_NE(root_frame, nullptr);
    auto *leaf = BPlusTreeLeafPage<int, RID>::Cast(root_frame);
    EXPECT_EQ(leaf->GetSize(), 8);

    std::vector<int> expected_sorted = {10, 20, 30, 40, 50, 60, 70, 80};
    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(leaf->KeyAt(i), expected_sorted[i]);
        EXPECT_EQ(leaf->ValueAt(i), RID(1, static_cast<uint32_t>(expected_sorted[i])));
    }
    bpm_->UnpinPage(tree.GetRootPageId(), false);
    AssertAllPagesUnpinned();

    // Verify GetValue finds every key
    for (int key : expected_sorted) {
        RID found;
        EXPECT_TRUE(tree.GetValue(key, &found));
        EXPECT_EQ(found, RID(1, static_cast<uint32_t>(key)));
        AssertAllPagesUnpinned();
    }
}

// ============================================================================
// Test 5: Duplicate Key Rejection & State Invariance
// ============================================================================
TEST_F(BPlusTreeTest, HandleDuplicateKeys) {
    BPlusTree<int, RID> tree("duplicate_keys_tree", bpm_.get(), std::less<int>{}, 10, 10);

    // 1. Initial insert into empty tree
    EXPECT_TRUE(tree.Insert(100, RID(1, 100)));
    EXPECT_EQ(tree.GetRecordCount(), 1u);

    // 2. Immediate duplicate insert on root leaf
    EXPECT_FALSE(tree.Insert(100, RID(999, 999))); // Different RID, same key
    EXPECT_EQ(tree.GetRecordCount(), 1u);
    AssertAllPagesUnpinned();

    // Verify existing key's value was NOT overwritten
    RID val;
    EXPECT_TRUE(tree.GetValue(100, &val));
    EXPECT_EQ(val, RID(1, 100));
    AssertAllPagesUnpinned();

    // 3. Insert additional keys into leaf with space
    EXPECT_TRUE(tree.Insert(50, RID(1, 50)));
    EXPECT_TRUE(tree.Insert(150, RID(1, 150)));
    EXPECT_TRUE(tree.Insert(25, RID(1, 25)));
    EXPECT_TRUE(tree.Insert(75, RID(1, 75)));
    EXPECT_EQ(tree.GetRecordCount(), 5u);
    AssertAllPagesUnpinned();

    // 4. Attempt duplicate inserts for every single existing key
    std::vector<int> existing_keys = {25, 50, 75, 100, 150};
    for (int k : existing_keys) {
        EXPECT_FALSE(tree.Insert(k, RID(888, 888)))
            << "Tree erroneously accepted duplicate key " << k;
        EXPECT_EQ(tree.GetRecordCount(), 5u);
        AssertAllPagesUnpinned();

        // Verify original value unchanged
        RID retrieved;
        EXPECT_TRUE(tree.GetValue(k, &retrieved));
        EXPECT_EQ(retrieved, RID(1, static_cast<uint32_t>(k)));
        AssertAllPagesUnpinned();
    }

    // 5. Verify non-duplicate insert still succeeds
    EXPECT_TRUE(tree.Insert(60, RID(1, 60)));
    EXPECT_EQ(tree.GetRecordCount(), 6u);
    AssertAllPagesUnpinned();

    // Attempt duplicate for 60
    EXPECT_FALSE(tree.Insert(60, RID(777, 777)));
    EXPECT_EQ(tree.GetRecordCount(), 6u);
    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 6: Leaf Split (Move Upper Half, Fix Next-Pointer Chain, Push Separator Up)
// ============================================================================
TEST_F(BPlusTreeTest, LeafSplitNextPointerAndSeparatorPushUp) {
    // Leaf max size = 4, internal max size = 4
    BPlusTree<int, RID> tree("leaf_split_tree", bpm_.get(), std::less<int>{}, 4, 4);

    // 1. Fill leaf to capacity (4 entries)
    EXPECT_TRUE(tree.Insert(10, RID(1, 10)));
    EXPECT_TRUE(tree.Insert(20, RID(1, 20)));
    EXPECT_TRUE(tree.Insert(30, RID(1, 30)));
    EXPECT_TRUE(tree.Insert(40, RID(1, 40)));
    EXPECT_EQ(tree.GetDepth(), 1u);
    EXPECT_EQ(tree.GetRecordCount(), 4u);

    page_id_t initial_leaf_pid = tree.GetRootPageId();
    AssertAllPagesUnpinned();

    // 2. Insert 5th element -> triggers leaf split and root internal node creation!
    EXPECT_TRUE(tree.Insert(50, RID(1, 50)));
    EXPECT_EQ(tree.GetDepth(), 2u);
    EXPECT_EQ(tree.GetRecordCount(), 5u);

    // 3. Inspect internal root node
    page_id_t root_pid = tree.GetRootPageId();
    EXPECT_NE(root_pid, initial_leaf_pid);

    Page *root_frame = bpm_->FetchPage(root_pid);
    ASSERT_NE(root_frame, nullptr);
    auto *root_internal = BPlusTreeInternalPage<int, page_id_t>::Cast(root_frame);
    EXPECT_TRUE(root_internal->IsInternalPage());
    EXPECT_EQ(root_internal->GetSize(), 2);

    // Separator key pushed up must be 30 (first key of right sibling leaf)
    EXPECT_EQ(root_internal->ValueAt(0), initial_leaf_pid);
    EXPECT_EQ(root_internal->KeyAt(1), 30);
    page_id_t right_leaf_pid = root_internal->ValueAt(1);
    EXPECT_NE(right_leaf_pid, initial_leaf_pid);
    bpm_->UnpinPage(root_pid, false);

    // 4. Inspect left leaf: must retain lower half (10, 20)
    Page *left_frame = bpm_->FetchPage(initial_leaf_pid);
    ASSERT_NE(left_frame, nullptr);
    auto *left_leaf = BPlusTreeLeafPage<int, RID>::Cast(left_frame);
    EXPECT_EQ(left_leaf->GetSize(), 2);
    EXPECT_EQ(left_leaf->KeyAt(0), 10);
    EXPECT_EQ(left_leaf->KeyAt(1), 20);

    // 5. Inspect next-pointer chain: left_leaf -> right_leaf -> INVALID_PAGE_ID
    EXPECT_EQ(left_leaf->GetNextPageId(), right_leaf_pid);
    bpm_->UnpinPage(initial_leaf_pid, false);

    // 6. Inspect right leaf: must contain upper half (30, 40, 50)
    Page *right_frame = bpm_->FetchPage(right_leaf_pid);
    ASSERT_NE(right_frame, nullptr);
    auto *right_leaf = BPlusTreeLeafPage<int, RID>::Cast(right_frame);
    EXPECT_EQ(right_leaf->GetSize(), 3);
    EXPECT_EQ(right_leaf->KeyAt(0), 30);
    EXPECT_EQ(right_leaf->KeyAt(1), 40);
    EXPECT_EQ(right_leaf->KeyAt(2), 50);
    EXPECT_EQ(right_leaf->GetNextPageId(), INVALID_PAGE_ID);
    bpm_->UnpinPage(right_leaf_pid, false);

    // 7. Test sequential leaf chain scan from left to right
    page_id_t scan_pid = initial_leaf_pid;
    std::vector<int> scanned_keys;
    while (scan_pid != INVALID_PAGE_ID) {
        Page *p = bpm_->FetchPage(scan_pid);
        auto *leaf = BPlusTreeLeafPage<int, RID>::Cast(p);
        for (int i = 0; i < leaf->GetSize(); ++i) {
            scanned_keys.push_back(leaf->KeyAt(i));
        }
        page_id_t next = leaf->GetNextPageId();
        bpm_->UnpinPage(scan_pid, false);
        scan_pid = next;
    }
    std::vector<int> expected_keys = {10, 20, 30, 40, 50};
    EXPECT_EQ(scanned_keys, expected_keys);

    // 8. Verify GetValue finds all keys via hierarchical traversal
    for (int k : expected_keys) {
        RID res;
        EXPECT_TRUE(tree.GetValue(k, &res));
        EXPECT_EQ(res, RID(1, static_cast<uint32_t>(k)));
    }

    std::string err;
    EXPECT_TRUE(tree.CheckIntegrity(&err)) << "Test 6 integrity failed: " << err;

    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 7: Manually Constructed Multi-Level Tree (Traverse Root to Leaf)
// ============================================================================
TEST_F(BPlusTreeTest, ManualMultiLevelTreeTraversal) {
    using LeafPage = BPlusTreeLeafPage<int, RID>;
    using InternalPage = BPlusTreeInternalPage<int, page_id_t>;

    page_id_t leaf1_pid = INVALID_PAGE_ID;
    page_id_t leaf2_pid = INVALID_PAGE_ID;
    page_id_t leaf3_pid = INVALID_PAGE_ID;
    page_id_t root_pid = INVALID_PAGE_ID;

    // 1. Allocate 3 leaf pages
    Page *leaf1_frame = bpm_->NewPage(&leaf1_pid);
    Page *leaf2_frame = bpm_->NewPage(&leaf2_pid);
    Page *leaf3_frame = bpm_->NewPage(&leaf3_pid);
    ASSERT_NE(leaf1_frame, nullptr);
    ASSERT_NE(leaf2_frame, nullptr);
    ASSERT_NE(leaf3_frame, nullptr);

    auto *leaf1 = LeafPage::Cast(leaf1_frame);
    auto *leaf2 = LeafPage::Cast(leaf2_frame);
    auto *leaf3 = LeafPage::Cast(leaf3_frame);

    leaf1->Init(leaf1_pid, INVALID_PAGE_ID, 10);
    leaf2->Init(leaf2_pid, INVALID_PAGE_ID, 10);
    leaf3->Init(leaf3_pid, INVALID_PAGE_ID, 10);

    // Leaf 1: keys 10, 20
    leaf1->Insert(10, RID(10, 1));
    leaf1->Insert(20, RID(10, 2));
    leaf1->SetNextPageId(leaf2_pid);

    // Leaf 2: keys 30, 40
    leaf2->Insert(30, RID(20, 1));
    leaf2->Insert(40, RID(20, 2));
    leaf2->SetNextPageId(leaf3_pid);

    // Leaf 3: keys 50, 60
    leaf3->Insert(50, RID(30, 1));
    leaf3->Insert(60, RID(30, 2));
    leaf3->SetNextPageId(INVALID_PAGE_ID);

    // 2. Allocate root internal page
    Page *root_frame = bpm_->NewPage(&root_pid);
    ASSERT_NE(root_frame, nullptr);
    auto *root_internal = InternalPage::Cast(root_frame);
    root_internal->Init(root_pid, INVALID_PAGE_ID, 10);

    // Populate internal node:
    // keys < 30 -> leaf 1
    // 30 <= keys < 50 -> leaf 2
    // keys >= 50 -> leaf 3
    root_internal->SetValueAt(0, leaf1_pid);
    root_internal->SetKeyAt(1, 30);
    root_internal->SetValueAt(1, leaf2_pid);
    root_internal->SetKeyAt(2, 50);
    root_internal->SetValueAt(2, leaf3_pid);
    root_internal->SetSize(3);

    leaf1->SetParentPageId(root_pid);
    leaf2->SetParentPageId(root_pid);
    leaf3->SetParentPageId(root_pid);

    // Unpin all pages
    bpm_->UnpinPage(leaf1_pid, true);
    bpm_->UnpinPage(leaf2_pid, true);
    bpm_->UnpinPage(leaf3_pid, true);
    bpm_->UnpinPage(root_pid, true);

    AssertAllPagesUnpinned();

    // 3. Construct BPlusTree and point to root_pid
    BPlusTree<int, RID> tree("manual_tree", bpm_.get());
    tree.SetRootPageId(root_pid);

    AssertAllPagesUnpinned();

    // 4. Test GetValue across all three leaves
    RID res;

    // Keys in Leaf 1
    EXPECT_TRUE(tree.GetValue(10, &res));
    EXPECT_EQ(res, RID(10, 1));
    AssertAllPagesUnpinned();

    EXPECT_TRUE(tree.GetValue(20, &res));
    EXPECT_EQ(res, RID(10, 2));
    AssertAllPagesUnpinned();

    // Keys in Leaf 2
    EXPECT_TRUE(tree.GetValue(30, &res));
    EXPECT_EQ(res, RID(20, 1));
    AssertAllPagesUnpinned();

    EXPECT_TRUE(tree.GetValue(40, &res));
    EXPECT_EQ(res, RID(20, 2));
    AssertAllPagesUnpinned();

    // Keys in Leaf 3
    EXPECT_TRUE(tree.GetValue(50, &res));
    EXPECT_EQ(res, RID(30, 1));
    AssertAllPagesUnpinned();

    EXPECT_TRUE(tree.GetValue(60, &res));
    EXPECT_EQ(res, RID(30, 2));
    AssertAllPagesUnpinned();

    // Absent keys in every range
    EXPECT_FALSE(tree.GetValue(5, &res));
    AssertAllPagesUnpinned();

    EXPECT_FALSE(tree.GetValue(25, &res));
    AssertAllPagesUnpinned();

    EXPECT_FALSE(tree.GetValue(35, &res));
    AssertAllPagesUnpinned();

    EXPECT_FALSE(tree.GetValue(45, &res));
    AssertAllPagesUnpinned();

    EXPECT_FALSE(tree.GetValue(100, &res));
    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 8: Dynamic Insert, Splits, and Large Scale GetValue Lookups
// ============================================================================
TEST_F(BPlusTreeTest, DynamicInsertAndGetValueStress) {
    // Small capacities to trigger frequent leaf and internal splits
    BPlusTree<int, RID> tree("stress_tree", bpm_.get(), std::less<int>{}, 4, 4);

    const int num_keys = 200;
    std::vector<int> keys;
    for (int i = 0; i < num_keys; ++i) {
        keys.push_back(i * 10);
    }

    // Shuffle insertion order
    std::mt19937 g(12345);
    std::shuffle(keys.begin(), keys.end(), g);

    for (int k : keys) {
        EXPECT_TRUE(tree.Insert(k, RID(static_cast<uint32_t>(k / 10), static_cast<uint32_t>(k % 100))));
    }

    // Duplicate insert should fail
    EXPECT_FALSE(tree.Insert(keys[0], RID(999, 999)));

    // Verify all buffer frames are unpinned after inserts
    AssertAllPagesUnpinned();

    // Verify tree depth has increased beyond 1
    EXPECT_GT(tree.GetDepth(), 1u);

    // Verify tree integrity
    std::string err;
    EXPECT_TRUE(tree.CheckIntegrity(&err)) << "Stress tree integrity failed: " << err;

    // Point lookup for EVERY inserted key
    for (int k : keys) {
        RID found_rid;
        ASSERT_TRUE(tree.GetValue(k, &found_rid)) << "Failed to find inserted key " << k;
        EXPECT_EQ(found_rid, RID(static_cast<uint32_t>(k / 10), static_cast<uint32_t>(k % 100)));
    }

    // Verify all pages remain unpinned after 200 successful lookups
    AssertAllPagesUnpinned();

    // Point lookup for missing keys (e.g. odd keys)
    for (int i = 0; i < num_keys; ++i) {
        int missing_key = (i * 10) + 5;
        RID found_rid;
        EXPECT_FALSE(tree.GetValue(missing_key, &found_rid)) << "Found non-existent key " << missing_key;
    }

    // Verify all pages remain unpinned after 200 failed lookups
    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 9: Vector Overload Compatibility (CMU 15-445 Parity)
// ============================================================================
TEST_F(BPlusTreeTest, VectorGetValueOverload) {
    BPlusTree<int, RID> tree("vector_test", bpm_.get(), std::less<int>{}, 5, 5);

    tree.Insert(100, RID(10, 1));
    tree.Insert(200, RID(20, 2));

    std::vector<RID> result;
    EXPECT_TRUE(tree.GetValue(100, &result));
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], RID(10, 1));

    EXPECT_TRUE(tree.GetValue(200, &result));
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], RID(20, 2));

    EXPECT_FALSE(tree.GetValue(300, &result));
    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 10: GenericKey and GenericComparator Integration
// ============================================================================
TEST_F(BPlusTreeTest, GenericKeyGetValue) {
    using Key64 = GenericKey<8>;
    BPlusTree<Key64, RID, GenericComparator<8>> tree("generic_key_tree", bpm_.get(), GenericComparator<8>{}, 4, 4);

    std::vector<int64_t> signed_keys = {
        -50000, -10000, -500, -1, 0, 1, 50, 2000, 75000, 1000000
    };

    for (int64_t sk : signed_keys) {
        EXPECT_TRUE(tree.Insert(Key64::From(sk), RID(1, static_cast<uint32_t>(sk & 0xFFFF))));
    }

    AssertAllPagesUnpinned();

    // Verify every signed key is correctly retrieved
    for (int64_t sk : signed_keys) {
        RID found;
        ASSERT_TRUE(tree.GetValue(Key64::From(sk), &found));
        EXPECT_EQ(found, RID(1, static_cast<uint32_t>(sk & 0xFFFF)));
    }

    // Negative lookup for absent key
    RID found;
    EXPECT_FALSE(tree.GetValue(Key64::From(-99999LL), &found));
    EXPECT_FALSE(tree.GetValue(Key64::From(999LL), &found));

    // Verify tree integrity
    std::string err;
    EXPECT_TRUE(tree.CheckIntegrity(&err)) << "GenericKey integrity failed: " << err;

    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 11: Persistence and Crash Restart
// ============================================================================
TEST_F(BPlusTreeTest, PersistenceAndRecovery) {
    page_id_t header_pid = INVALID_PAGE_ID;

    // Phase 1: Build tree and flush to disk
    {
        auto disk_mgr = std::make_unique<DiskManager>(db_file_);
        auto bpm = std::make_unique<BufferPoolManager>(20, disk_mgr.get());

        BPlusTree<int, RID> tree("persist_tree", bpm.get(), std::less<int>{}, 4, 4);
        header_pid = tree.GetHeaderPageId();
        EXPECT_NE(header_pid, INVALID_PAGE_ID);

        for (int i = 1; i <= 50; ++i) {
            EXPECT_TRUE(tree.Insert(i * 10, RID(1, static_cast<uint32_t>(i))));
        }

        // Flush all pages to disk
        bpm->FlushAllPages();
    } // BufferPoolManager and DiskManager destroyed

    // Phase 2: Restart engine from disk file and verify GetValue works
    {
        auto disk_mgr = std::make_unique<DiskManager>(db_file_);
        auto bpm = std::make_unique<BufferPoolManager>(20, disk_mgr.get());

        BPlusTree<int, RID> tree("persist_tree", header_pid, bpm.get(), std::less<int>{}, 4, 4);

        EXPECT_FALSE(tree.IsEmpty());
        EXPECT_EQ(tree.GetRecordCount(), 50u);

        for (int i = 1; i <= 50; ++i) {
            RID found;
            ASSERT_TRUE(tree.GetValue(i * 10, &found));
            EXPECT_EQ(found, RID(1, static_cast<uint32_t>(i)));
        }

        RID missing;
        EXPECT_FALSE(tree.GetValue(9999, &missing));

        std::string err;
        EXPECT_TRUE(tree.CheckIntegrity(&err)) << "Post-recovery integrity failed: " << err;

        for (size_t i = 0; i < bpm->GetPoolSize(); ++i) {
            Page *page = bpm->GetPage(static_cast<frame_id_t>(i));
            if (page != nullptr) {
                EXPECT_EQ(page->GetPinCount(), 0);
            }
        }
    }
}

// ============================================================================
// Test 12: Direct Root Split and Header Metadata Update
// ============================================================================
TEST_F(BPlusTreeTest, DirectRootSplitAndHeaderUpdate) {
    using LeafPage = BPlusTreeLeafPage<int, RID>;
    using InternalPage = BPlusTreeInternalPage<int, page_id_t>;

    BPlusTree<int, RID> tree("direct_root_split_tree", bpm_.get(), std::less<int>{}, 4, 4);

    // 1. Allocate old root leaf and new sibling leaf
    page_id_t left_pid = INVALID_PAGE_ID;
    page_id_t right_pid = INVALID_PAGE_ID;
    Page *left_frame = bpm_->NewPage(&left_pid);
    Page *right_frame = bpm_->NewPage(&right_pid);
    ASSERT_NE(left_frame, nullptr);
    ASSERT_NE(right_frame, nullptr);

    auto *left_leaf = LeafPage::Cast(left_frame);
    left_leaf->Init(left_pid, INVALID_PAGE_ID, 4);
    left_leaf->Insert(10, RID(1, 10));
    left_leaf->Insert(20, RID(1, 20));

    auto *right_leaf = LeafPage::Cast(right_frame);
    right_leaf->Init(right_pid, INVALID_PAGE_ID, 4);
    right_leaf->Insert(30, RID(1, 30));
    right_leaf->Insert(40, RID(1, 40));

    // Connect leaf chain
    left_leaf->SetNextPageId(right_pid);

    // Initial root setup
    tree.SetRootPageId(left_pid);
    EXPECT_EQ(tree.GetRootPageId(), left_pid);
    EXPECT_EQ(tree.GetDepth(), 1u);

    // 2. Perform CreateNewRoot (Root Split)
    page_id_t new_root_pid = tree.CreateNewRoot(left_leaf, 30, right_leaf);
    EXPECT_NE(new_root_pid, INVALID_PAGE_ID);
    EXPECT_NE(new_root_pid, left_pid);
    EXPECT_NE(new_root_pid, right_pid);

    // 3. Verify in-memory tree state and persistent header page
    EXPECT_EQ(tree.GetRootPageId(), new_root_pid);
    EXPECT_EQ(tree.GetDepth(), 2u);

    // Persistent header check
    Page *header_frame = bpm_->FetchPage(tree.GetHeaderPageId());
    ASSERT_NE(header_frame, nullptr);
    auto *header = BPlusTreeHeaderPage::Cast(header_frame);
    EXPECT_EQ(header->GetRootPageId(), new_root_pid);
    EXPECT_EQ(header->GetDepth(), 2u);
    bpm_->UnpinPage(tree.GetHeaderPageId(), false);

    // 4. Verify parent pointers on both children were updated to new_root_pid
    EXPECT_EQ(left_leaf->GetParentPageId(), new_root_pid);
    EXPECT_EQ(right_leaf->GetParentPageId(), new_root_pid);

    // 5. Inspect new root internal page
    Page *root_frame = bpm_->FetchPage(new_root_pid);
    ASSERT_NE(root_frame, nullptr);
    auto *root_internal = InternalPage::Cast(root_frame);
    EXPECT_TRUE(root_internal->IsInternalPage());
    EXPECT_EQ(root_internal->GetSize(), 2);
    EXPECT_EQ(root_internal->ValueAt(0), left_pid);
    EXPECT_EQ(root_internal->KeyAt(1), 30);
    EXPECT_EQ(root_internal->ValueAt(1), right_pid);
    EXPECT_EQ(root_internal->GetParentPageId(), INVALID_PAGE_ID);
    bpm_->UnpinPage(new_root_pid, false);

    // Unpin leaf pages
    bpm_->UnpinPage(left_pid, true);
    bpm_->UnpinPage(right_pid, true);

    // 6. Verify hierarchical GetValue works through the new root
    RID found;
    EXPECT_TRUE(tree.GetValue(10, &found));
    EXPECT_EQ(found, RID(1, 10));
    EXPECT_TRUE(tree.GetValue(30, &found));
    EXPECT_EQ(found, RID(1, 30));
    EXPECT_FALSE(tree.GetValue(99, &found));

    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 13: Direct Internal Node Split and Child Reparenting
// ============================================================================
TEST_F(BPlusTreeTest, DirectInternalNodeSplitAndReparenting) {
    using LeafPage = BPlusTreeLeafPage<int, RID>;
    using InternalPage = BPlusTreeInternalPage<int, page_id_t>;

    BPlusTree<int, RID> tree("direct_internal_split_tree", bpm_.get(), std::less<int>{}, 4, 4);

    // 1. Allocate 4 children pages (P0, P1, P2, P3)
    std::vector<page_id_t> child_pids(4);
    for (int i = 0; i < 4; ++i) {
        Page *cp = bpm_->NewPage(&child_pids[i]);
        ASSERT_NE(cp, nullptr);
        auto *leaf = LeafPage::Cast(cp);
        leaf->Init(child_pids[i], INVALID_PAGE_ID, 4);
        leaf->Insert((i + 1) * 10, RID(1, static_cast<uint32_t>((i + 1) * 10)));
        bpm_->UnpinPage(child_pids[i], true);
    }

    // 2. Allocate an internal node and fill to capacity (size = 4):
    // array_[0] = (dummy, P0)
    // array_[1] = (20, P1)
    // array_[2] = (30, P2)
    // array_[3] = (40, P3)
    page_id_t internal_pid = INVALID_PAGE_ID;
    Page *internal_frame = bpm_->NewPage(&internal_pid);
    ASSERT_NE(internal_frame, nullptr);
    auto *internal = InternalPage::Cast(internal_frame);
    internal->Init(internal_pid, INVALID_PAGE_ID, 4);
    internal->SetValueAt(0, child_pids[0]);
    internal->SetKeyAt(1, 20);
    internal->SetValueAt(1, child_pids[1]);
    internal->SetKeyAt(2, 30);
    internal->SetValueAt(2, child_pids[2]);
    internal->SetKeyAt(3, 40);
    internal->SetValueAt(3, child_pids[3]);
    internal->SetSize(4);

    // Set child parent pointers to internal_pid
    for (int i = 0; i < 4; ++i) {
        Page *cp = bpm_->FetchPage(child_pids[i]);
        LeafPage::Cast(cp)->SetParentPageId(internal_pid);
        bpm_->UnpinPage(child_pids[i], true);
    }

    // 3. Call tree.Split(internal)
    InternalPage *new_internal = tree.Split(internal);
    ASSERT_NE(new_internal, nullptr);
    page_id_t new_internal_pid = new_internal->GetPageId();
    EXPECT_NE(new_internal_pid, internal_pid);

    // 4. Verify distribution of entries:
    // Left internal retains lower half (mid = 2 entries: P0 and (20, P1))
    EXPECT_EQ(internal->GetSize(), 2);
    EXPECT_EQ(internal->ValueAt(0), child_pids[0]);
    EXPECT_EQ(internal->KeyAt(1), 20);
    EXPECT_EQ(internal->ValueAt(1), child_pids[1]);

    // Right internal receives upper half (2 entries: (30, P2) as entry 0, and (40, P3) as entry 1)
    EXPECT_EQ(new_internal->GetSize(), 2);
    EXPECT_EQ(new_internal->KeyAt(0), 30); // KeyAt(0) holds median split key for push-up
    EXPECT_EQ(new_internal->ValueAt(0), child_pids[2]);
    EXPECT_EQ(new_internal->KeyAt(1), 40);
    EXPECT_EQ(new_internal->ValueAt(1), child_pids[3]);

    // 5. Verify child reparenting:
    // P0 and P1 must still point to internal_pid
    for (int i = 0; i < 2; ++i) {
        Page *cp = bpm_->FetchPage(child_pids[i]);
        EXPECT_EQ(LeafPage::Cast(cp)->GetParentPageId(), internal_pid);
        bpm_->UnpinPage(child_pids[i], false);
    }

    // P2 and P3 must now point to new_internal_pid!
    for (int i = 2; i < 4; ++i) {
        Page *cp = bpm_->FetchPage(child_pids[i]);
        EXPECT_EQ(LeafPage::Cast(cp)->GetParentPageId(), new_internal_pid);
        bpm_->UnpinPage(child_pids[i], false);
    }

    // Unpin internal pages
    bpm_->UnpinPage(new_internal_pid, true);
    bpm_->UnpinPage(internal_pid, true);

    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 14: Full Dynamic Internal Split & Root Split Depth Progression
// ============================================================================
TEST_F(BPlusTreeTest, InternalSplitAndRootSplitDepthProgression) {
    // Ultra-compact node sizes to force frequent internal splits and root splits:
    // leaf_max_size = 3, internal_max_size = 3
    BPlusTree<int, RID> tree("progression_tree", bpm_.get(), std::less<int>{}, 3, 3);

    EXPECT_EQ(tree.GetDepth(), 0u);

    // 1. Insert 3 elements -> single leaf root (depth = 1)
    tree.Insert(10, RID(1, 10));
    tree.Insert(20, RID(1, 20));
    tree.Insert(30, RID(1, 30));
    EXPECT_EQ(tree.GetDepth(), 1u);

    // 2. Insert 4th element -> leaf split + root split (depth becomes 2)
    tree.Insert(40, RID(1, 40));
    EXPECT_EQ(tree.GetDepth(), 2u);
    AssertAllPagesUnpinned();

    // 3. Continue inserting to fill internal root:
    // At leaf_max_size = 3, internal_max_size = 3:
    // Internal root can hold at most 3 child pointers.
    // When 4th child pointer is pushed up, internal root splits!
    // Internal root split creates a new level-3 root!
    for (int k = 50; k <= 120; k += 10) {
        EXPECT_TRUE(tree.Insert(k, RID(1, static_cast<uint32_t>(k))));
    }

    EXPECT_GE(tree.GetDepth(), 3u);
    EXPECT_EQ(tree.GetRecordCount(), 12u);
    AssertAllPagesUnpinned();

    // Verify all keys from 10 to 120 can be accurately retrieved
    for (int k = 10; k <= 120; k += 10) {
        RID res;
        ASSERT_TRUE(tree.GetValue(k, &res)) << "Key " << k << " missing after internal splits";
        EXPECT_EQ(res, RID(1, static_cast<uint32_t>(k)));
    }

    // Verify root ID in header matches tree's root ID
    Page *header_frame = bpm_->FetchPage(tree.GetHeaderPageId());
    ASSERT_NE(header_frame, nullptr);
    auto *header = BPlusTreeHeaderPage::Cast(header_frame);
    EXPECT_EQ(header->GetRootPageId(), tree.GetRootPageId());
    EXPECT_EQ(header->GetDepth(), tree.GetDepth());
    bpm_->UnpinPage(tree.GetHeaderPageId(), false);

    std::string err;
    EXPECT_TRUE(tree.CheckIntegrity(&err)) << "Test 14 integrity failed: " << err;

    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 15: Ascending Inserts with Small Fanout (3, 3) Forcing Cascading Right Splits
// ============================================================================
TEST_F(BPlusTreeTest, AscendingInsertsSmallFanoutSplits) {
    // Ultra-small fanouts: leaf_max_size = 3, internal_max_size = 3
    BPlusTree<int, RID> tree("asc_small_fanout_tree", bpm_.get(), std::less<int>{}, 3, 3);

    const int num_keys = 120;
    for (int i = 1; i <= num_keys; ++i) {
        bool ok = tree.Insert(i, RID(static_cast<uint32_t>(i / 10), static_cast<uint32_t>(i)));
        ASSERT_TRUE(ok) << "Failed to insert ascending key " << i;
        std::string err;
        ASSERT_TRUE(tree.CheckIntegrity(&err)) << "After inserting " << i << ": " << err;
    }

    EXPECT_EQ(tree.GetRecordCount(), static_cast<uint64_t>(num_keys));
    EXPECT_GE(tree.GetDepth(), 4u); // Multiple cascading root splits occurred
    AssertAllPagesUnpinned();

    // 1. Verify point lookups for every single inserted key
    for (int i = 1; i <= num_keys; ++i) {
        RID found;
        ASSERT_TRUE(tree.GetValue(i, &found)) << "Failed to find ascending key " << i;
        EXPECT_EQ(found, RID(static_cast<uint32_t>(i / 10), static_cast<uint32_t>(i)));
    }
    AssertAllPagesUnpinned();

    // 2. Negative lookups
    RID missing;
    EXPECT_FALSE(tree.GetValue(0, &missing));
    EXPECT_FALSE(tree.GetValue(num_keys + 1, &missing));
    EXPECT_FALSE(tree.GetValue(-99, &missing));
    AssertAllPagesUnpinned();

    // 3. Sequential scan through next-pointer leaf chain
    Page *leaf_frame = tree.FindLeafPage(1, true); // Leftmost leaf
    ASSERT_NE(leaf_frame, nullptr);
    page_id_t curr_pid = leaf_frame->GetPageId();
    bpm_->UnpinPage(curr_pid, false);

    std::vector<int> scanned_keys;
    while (curr_pid != INVALID_PAGE_ID) {
        Page *p = bpm_->FetchPage(curr_pid);
        ASSERT_NE(p, nullptr);
        auto *leaf = BPlusTreeLeafPage<int, RID>::Cast(p);
        for (int i = 0; i < leaf->GetSize(); ++i) {
            scanned_keys.push_back(leaf->KeyAt(i));
        }
        page_id_t next_pid = leaf->GetNextPageId();
        bpm_->UnpinPage(curr_pid, false);
        curr_pid = next_pid;
    }

    ASSERT_EQ(scanned_keys.size(), static_cast<size_t>(num_keys));
    for (int i = 0; i < num_keys; ++i) {
        EXPECT_EQ(scanned_keys[i], i + 1);
    }

    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 16: Descending Inserts with Small Fanout (4, 3) Forcing Cascading Left Splits
// ============================================================================
TEST_F(BPlusTreeTest, DescendingInsertsSmallFanoutSplits) {
    // Ultra-small fanouts: leaf_max_size = 4, internal_max_size = 3
    BPlusTree<int, RID> tree("desc_small_fanout_tree", bpm_.get(), std::less<int>{}, 4, 3);

    const int num_keys = 120;
    // Insert descending: 120 down to 1
    for (int i = num_keys; i >= 1; --i) {
        bool ok = tree.Insert(i, RID(static_cast<uint32_t>(i), static_cast<uint32_t>(i * 2)));
        ASSERT_TRUE(ok) << "Failed to insert descending key " << i;
        std::string err;
        ASSERT_TRUE(tree.CheckIntegrity(&err)) << "After inserting " << i << ": " << err;
    }

    EXPECT_EQ(tree.GetRecordCount(), static_cast<uint64_t>(num_keys));
    EXPECT_GE(tree.GetDepth(), 4u);
    AssertAllPagesUnpinned();

    // 1. Verify point lookups for every single inserted key
    for (int i = 1; i <= num_keys; ++i) {
        RID found;
        ASSERT_TRUE(tree.GetValue(i, &found)) << "Failed to find descending key " << i;
        EXPECT_EQ(found, RID(static_cast<uint32_t>(i), static_cast<uint32_t>(i * 2)));
    }
    AssertAllPagesUnpinned();

    // 2. Sequential scan through next-pointer leaf chain from leftmost to rightmost
    Page *leaf_frame = tree.FindLeafPage(1, true); // Leftmost leaf
    ASSERT_NE(leaf_frame, nullptr);
    page_id_t curr_pid = leaf_frame->GetPageId();
    bpm_->UnpinPage(curr_pid, false);

    std::vector<int> scanned_keys;
    while (curr_pid != INVALID_PAGE_ID) {
        Page *p = bpm_->FetchPage(curr_pid);
        ASSERT_NE(p, nullptr);
        auto *leaf = BPlusTreeLeafPage<int, RID>::Cast(p);
        for (int i = 0; i < leaf->GetSize(); ++i) {
            scanned_keys.push_back(leaf->KeyAt(i));
        }
        page_id_t next_pid = leaf->GetNextPageId();
        bpm_->UnpinPage(curr_pid, false);
        curr_pid = next_pid;
    }

    // Must still produce perfectly sorted order 1 .. 120!
    ASSERT_EQ(scanned_keys.size(), static_cast<size_t>(num_keys));
    for (int i = 0; i < num_keys; ++i) {
        EXPECT_EQ(scanned_keys[i], i + 1);
    }

    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 17: Random Inserts with Small Fanouts (3, 4) Forcing Arbitrary Splits Across Levels
// ============================================================================
TEST_F(BPlusTreeTest, RandomInsertsSmallFanoutSplits) {
    // Ultra-small fanouts: leaf_max_size = 3, internal_max_size = 4
    BPlusTree<int, RID> tree("random_small_fanout_tree", bpm_.get(), std::less<int>{}, 3, 4);

    const int num_keys = 250;
    std::vector<int> keys(num_keys);
    for (int i = 0; i < num_keys; ++i) {
        keys[i] = (i + 1) * 3; // 3, 6, 9, ..., 750
    }

    std::mt19937 rng(424242);
    std::shuffle(keys.begin(), keys.end(), rng);

    // Insert all keys in random order
    for (int k : keys) {
        bool ok = tree.Insert(k, RID(static_cast<uint32_t>(k % 100), static_cast<uint32_t>(k)));
        ASSERT_TRUE(ok) << "Failed to insert random key " << k;
        std::string err;
        ASSERT_TRUE(tree.CheckIntegrity(&err)) << "After inserting " << k << ": " << err;
    }

    EXPECT_EQ(tree.GetRecordCount(), static_cast<uint64_t>(num_keys));
    EXPECT_GE(tree.GetDepth(), 4u);
    AssertAllPagesUnpinned();

    // 1. Verify all inserted keys are found with exact values
    for (int k : keys) {
        RID found;
        ASSERT_TRUE(tree.GetValue(k, &found)) << "Failed to find random key " << k;
        EXPECT_EQ(found, RID(static_cast<uint32_t>(k % 100), static_cast<uint32_t>(k)));
    }
    AssertAllPagesUnpinned();

    // 2. Duplicate rejection verification on random keys
    for (size_t i = 0; i < 20; ++i) {
        int dup_key = keys[i];
        EXPECT_FALSE(tree.Insert(dup_key, RID(999, 999)))
            << "Accepted duplicate key " << dup_key;
        EXPECT_EQ(tree.GetRecordCount(), static_cast<uint64_t>(num_keys));
    }
    AssertAllPagesUnpinned();

    // 3. Verify leaf chain scan visits keys in sorted order 3, 6, 9, ...
    std::vector<int> sorted_keys = keys;
    std::sort(sorted_keys.begin(), sorted_keys.end());

    Page *leftmost_frame = tree.FindLeafPage(sorted_keys.front(), true);
    ASSERT_NE(leftmost_frame, nullptr);
    page_id_t curr_pid = leftmost_frame->GetPageId();
    bpm_->UnpinPage(curr_pid, false);

    std::vector<int> scanned_keys;
    while (curr_pid != INVALID_PAGE_ID) {
        Page *p = bpm_->FetchPage(curr_pid);
        ASSERT_NE(p, nullptr);
        auto *leaf = BPlusTreeLeafPage<int, RID>::Cast(p);
        for (int i = 0; i < leaf->GetSize(); ++i) {
            scanned_keys.push_back(leaf->KeyAt(i));
        }
        page_id_t next_pid = leaf->GetNextPageId();
        bpm_->UnpinPage(curr_pid, false);
        curr_pid = next_pid;
    }

    ASSERT_EQ(scanned_keys.size(), static_cast<size_t>(num_keys));
    EXPECT_EQ(scanned_keys, sorted_keys);

    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 18: Alternating Min/Max Ping-Pong Inserts with Small Fanout (4, 4)
// ============================================================================
TEST_F(BPlusTreeTest, PingPongInsertsSmallFanoutSplits) {
    // Small fanout 4, 4
    BPlusTree<int, RID> tree("ping_pong_tree", bpm_.get(), std::less<int>{}, 4, 4);

    const int half_count = 80;
    std::vector<int> inserted_keys;

    // Alternately insert from low end and high end:
    // low: 1, 2, ..., 80; high: 1000, 999, ..., 921
    for (int i = 1; i <= half_count; ++i) {
        int low_key = i;
        int high_key = 1001 - i;

        ASSERT_TRUE(tree.Insert(low_key, RID(1, static_cast<uint32_t>(low_key))));
        inserted_keys.push_back(low_key);
        std::string err;
        ASSERT_TRUE(tree.CheckIntegrity(&err)) << "After inserting low " << low_key << ": " << err;

        ASSERT_TRUE(tree.Insert(high_key, RID(2, static_cast<uint32_t>(high_key))));
        inserted_keys.push_back(high_key);
        ASSERT_TRUE(tree.CheckIntegrity(&err)) << "After inserting high " << high_key << ": " << err;
    }

    EXPECT_EQ(tree.GetRecordCount(), static_cast<uint64_t>(half_count * 2));
    EXPECT_GE(tree.GetDepth(), 3u);
    AssertAllPagesUnpinned();

    // Verify all keys
    for (int k : inserted_keys) {
        RID found;
        ASSERT_TRUE(tree.GetValue(k, &found)) << "Failed to find ping-pong key " << k;
        uint32_t expected_page = (k <= half_count) ? 1 : 2;
        EXPECT_EQ(found, RID(expected_page, static_cast<uint32_t>(k)));
    }
    AssertAllPagesUnpinned();

    // Leaf chain scan
    std::sort(inserted_keys.begin(), inserted_keys.end());
    Page *leaf_frame = tree.FindLeafPage(inserted_keys.front(), true);
    ASSERT_NE(leaf_frame, nullptr);
    page_id_t curr_pid = leaf_frame->GetPageId();
    bpm_->UnpinPage(curr_pid, false);

    std::vector<int> scanned_keys;
    while (curr_pid != INVALID_PAGE_ID) {
        Page *p = bpm_->FetchPage(curr_pid);
        ASSERT_NE(p, nullptr);
        auto *leaf = BPlusTreeLeafPage<int, RID>::Cast(p);
        for (int i = 0; i < leaf->GetSize(); ++i) {
            scanned_keys.push_back(leaf->KeyAt(i));
        }
        page_id_t next_pid = leaf->GetNextPageId();
        bpm_->UnpinPage(curr_pid, false);
        curr_pid = next_pid;
    }

    EXPECT_EQ(scanned_keys, inserted_keys);
    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 19: Tree Validator on Empty Tree and Single Leaf Root
// ============================================================================
TEST_F(BPlusTreeTest, TreeValidatorOnEmptyAndSingleNode) {
    BPlusTree<int, RID> tree("validator_empty_single", bpm_.get(), std::less<int>{}, 4, 4);

    // 1. Empty tree validation
    TreeValidationResult empty_res = tree.ValidateTree();
    EXPECT_TRUE(empty_res.is_valid);
    EXPECT_TRUE(empty_res.error_message.empty());
    EXPECT_EQ(empty_res.total_keys, 0u);
    EXPECT_EQ(empty_res.leaf_page_count, 0u);
    EXPECT_EQ(empty_res.internal_page_count, 0u);
    EXPECT_EQ(empty_res.depth, 0u);
    EXPECT_TRUE(tree.CheckIntegrity());
    AssertAllPagesUnpinned();

    // 2. Single leaf node with elements
    EXPECT_TRUE(tree.Insert(10, RID(1, 10)));
    EXPECT_TRUE(tree.Insert(20, RID(1, 20)));
    EXPECT_TRUE(tree.Insert(30, RID(1, 30)));

    TreeValidationResult single_res = tree.ValidateTree();
    EXPECT_TRUE(single_res.is_valid);
    EXPECT_TRUE(single_res.error_message.empty());
    EXPECT_EQ(single_res.total_keys, 3u);
    EXPECT_EQ(single_res.leaf_page_count, 1u);
    EXPECT_EQ(single_res.internal_page_count, 0u);
    EXPECT_EQ(single_res.depth, 1u);
    EXPECT_TRUE(tree.CheckIntegrity());
    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 20: Tree Validator on Full Multi-Level Tree Structure
// ============================================================================
TEST_F(BPlusTreeTest, TreeValidatorOnMultiLevelTree) {
    // Small fanouts to build a multi-level tree with internal and leaf nodes
    BPlusTree<int, RID> tree("validator_multilevel", bpm_.get(), std::less<int>{}, 3, 3);

    for (int i = 1; i <= 60; ++i) {
        ASSERT_TRUE(tree.Insert(i * 5, RID(1, static_cast<uint32_t>(i))));
    }

    TreeValidationResult res = tree.ValidateTree();
    EXPECT_TRUE(res.is_valid);
    EXPECT_TRUE(res.error_message.empty()) << "Validation failed: " << res.error_message;
    EXPECT_EQ(res.total_keys, 60u);
    EXPECT_GE(res.depth, 3u);
    EXPECT_GT(res.leaf_page_count, 20u);
    EXPECT_GT(res.internal_page_count, 5u);

    std::string err;
    EXPECT_TRUE(tree.CheckIntegrity(&err));
    EXPECT_TRUE(err.empty());

    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 21: Tree Validator Accurately Detects Structural Violations
// ============================================================================
TEST_F(BPlusTreeTest, TreeValidatorCatchesViolations) {
    using LeafPage = BPlusTreeLeafPage<int, RID>;

    // 1. Violation A: Corrupted Parent Pointer
    {
        BPlusTree<int, RID> tree("tamper_parent", bpm_.get(), std::less<int>{}, 3, 3);
        for (int i = 1; i <= 10; ++i) {
            tree.Insert(i * 10, RID(1, static_cast<uint32_t>(i)));
        }
        EXPECT_TRUE(tree.CheckIntegrity());

        // Tamper with leftmost leaf's parent pointer
        Page *leaf_frame = tree.FindLeafPage(10, true);
        ASSERT_NE(leaf_frame, nullptr);
        auto *leaf = LeafPage::Cast(leaf_frame);
        page_id_t original_parent = leaf->GetParentPageId();
        leaf->SetParentPageId(9999); // Corrupt parent ID
        bpm_->UnpinPage(leaf_frame->GetPageId(), true);

        // Validator must catch parent pointer mismatch
        TreeValidationResult res = tree.ValidateTree();
        EXPECT_FALSE(res.is_valid);
        EXPECT_NE(res.error_message.find("Parent pointer mismatch"), std::string::npos);

        // Restore to avoid leaving corrupted state
        leaf_frame = bpm_->FetchPage(leaf->GetPageId());
        leaf->SetParentPageId(original_parent);
        bpm_->UnpinPage(leaf_frame->GetPageId(), true);
        EXPECT_TRUE(tree.CheckIntegrity());
        AssertAllPagesUnpinned();
    }

    // 2. Violation B: Corrupted In-Node Key Ordering
    {
        BPlusTree<int, RID> tree("tamper_keys", bpm_.get(), std::less<int>{}, 4, 4);
        tree.Insert(10, RID(1, 10));
        tree.Insert(20, RID(1, 20));
        tree.Insert(30, RID(1, 30));
        EXPECT_TRUE(tree.CheckIntegrity());

        Page *root_frame = bpm_->FetchPage(tree.GetRootPageId());
        auto *leaf = LeafPage::Cast(root_frame);
        leaf->SetKeyAt(0, 50); // Unsorted: 50, 20, 30
        bpm_->UnpinPage(tree.GetRootPageId(), true);

        TreeValidationResult res = tree.ValidateTree();
        EXPECT_FALSE(res.is_valid);
        EXPECT_NE(res.error_message.find("keys not strictly ascending"), std::string::npos);

        // Restore
        root_frame = bpm_->FetchPage(tree.GetRootPageId());
        leaf->SetKeyAt(0, 10);
        bpm_->UnpinPage(tree.GetRootPageId(), true);
        EXPECT_TRUE(tree.CheckIntegrity());
        AssertAllPagesUnpinned();
    }

    // 3. Violation C: Severed Sequential Leaf Chain
    {
        BPlusTree<int, RID> tree("tamper_chain", bpm_.get(), std::less<int>{}, 3, 3);
        for (int i = 1; i <= 8; ++i) {
            tree.Insert(i * 10, RID(1, static_cast<uint32_t>(i)));
        }
        EXPECT_TRUE(tree.CheckIntegrity());

        // Sever leaf chain from leftmost leaf
        Page *leaf_frame = tree.FindLeafPage(10, true);
        auto *leaf = LeafPage::Cast(leaf_frame);
        page_id_t original_next = leaf->GetNextPageId();
        leaf->SetNextPageId(INVALID_PAGE_ID);
        bpm_->UnpinPage(leaf_frame->GetPageId(), true);

        TreeValidationResult res = tree.ValidateTree();
        EXPECT_FALSE(res.is_valid);
        EXPECT_NE(res.error_message.find("Mismatch between leaves"), std::string::npos);

        // Restore
        leaf_frame = bpm_->FetchPage(leaf->GetPageId());
        leaf->SetNextPageId(original_next);
        bpm_->UnpinPage(leaf_frame->GetPageId(), true);
        EXPECT_TRUE(tree.CheckIntegrity());
        AssertAllPagesUnpinned();
    }
}

// ============================================================================
// Test 22: Graphviz DOT Dump String and File Output
// ============================================================================
TEST_F(BPlusTreeTest, GraphvizDumpOutputVerification) {
    BPlusTree<int, RID> tree("graphviz_demo_tree", bpm_.get(), std::less<int>{}, 3, 3);

    // 1. Empty tree DOT representation
    std::string empty_dot = tree.ToDot();
    EXPECT_NE(empty_dot.find("digraph BPlusTree"), std::string::npos);
    EXPECT_NE(empty_dot.find("Empty"), std::string::npos);
    AssertAllPagesUnpinned();

    // 2. Populate tree with data
    for (int i = 1; i <= 15; ++i) {
        tree.Insert(i * 10, RID(1, static_cast<uint32_t>(i)));
    }
    AssertAllPagesUnpinned();

    // 3. Multi-level tree DOT representation
    std::string dot = tree.ToDot();
    EXPECT_NE(dot.find("digraph BPlusTree"), std::string::npos);
    EXPECT_NE(dot.find("rankdir=TB"), std::string::npos);
    EXPECT_NE(dot.find("shape=record"), std::string::npos);
    EXPECT_NE(dot.find("Type: Internal"), std::string::npos);
    EXPECT_NE(dot.find("Type: Leaf"), std::string::npos);
    EXPECT_NE(dot.find("style=dashed"), std::string::npos); // Leaf chain
    EXPECT_NE(dot.find("color=\"#2E7D32\""), std::string::npos);

    // 4. Dump to disk file
    const std::string dot_file = "test_b_plus_tree.dot";
    if (fs::exists(dot_file)) {
        fs::remove(dot_file);
    }

    EXPECT_TRUE(tree.DumpGraphviz(dot_file));
    EXPECT_TRUE(fs::exists(dot_file));
    EXPECT_GT(fs::file_size(dot_file), 100u);

    // Clean up temporary file
    fs::remove(dot_file);
    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 23: Comprehensive Fanout Matrix Invariant Fuzzing
// ============================================================================
TEST_F(BPlusTreeTest, FuzzIntegrityAllFanouts) {
    std::vector<std::pair<int, int>> fanouts = {
        {3, 3}, {3, 4}, {4, 3}, {4, 4}, {4, 5}, {5, 4}, {5, 5}, {6, 6}, {7, 7}
    };

    for (auto [leaf_max, int_max] : fanouts) {
        std::string tree_name = "fuzz_" + std::to_string(leaf_max) + "_" + std::to_string(int_max);
        BPlusTree<int, RID> tree(tree_name, bpm_.get(), std::less<int>{}, leaf_max, int_max);

        // Pattern 1: Ascending
        for (int i = 1; i <= 60; ++i) {
            ASSERT_TRUE(tree.Insert(i, RID(1, static_cast<uint32_t>(i))));
            std::string err;
            ASSERT_TRUE(tree.CheckIntegrity(&err))
                << "Ascending fanout (" << leaf_max << "," << int_max << ") failed at " << i << ": " << err;
        }

        // Pattern 2: Ping Pong into existing
        for (int i = 1; i <= 30; ++i) {
            int k1 = 1000 + i;
            int k2 = 500 - i;
            ASSERT_TRUE(tree.Insert(k1, RID(2, static_cast<uint32_t>(k1))));
            std::string err;
            ASSERT_TRUE(tree.CheckIntegrity(&err))
                << "Ping-pong fanout (" << leaf_max << "," << int_max << ") failed at k1=" << k1 << ": " << err;

            ASSERT_TRUE(tree.Insert(k2, RID(2, static_cast<uint32_t>(k2))));
            ASSERT_TRUE(tree.CheckIntegrity(&err))
                << "Ping-pong fanout (" << leaf_max << "," << int_max << ") failed at k2=" << k2 << ": " << err;
        }

        // Pattern 3: Standalone Descending Tree
        {
            BPlusTree<int, RID> desc_tree("fuzz_desc_" + std::to_string(leaf_max) + "_" + std::to_string(int_max),
                                          bpm_.get(), std::less<int>{}, leaf_max, int_max);
            for (int i = 60; i >= 1; --i) {
                ASSERT_TRUE(desc_tree.Insert(i, RID(3, static_cast<uint32_t>(i))));
                std::string err;
                ASSERT_TRUE(desc_tree.CheckIntegrity(&err))
                    << "Descending fanout (" << leaf_max << "," << int_max << ") failed at " << i << ": " << err;
            }
        }

        // Pattern 4: Standalone Random Tree
        {
            BPlusTree<int, RID> rand_tree("fuzz_rand_" + std::to_string(leaf_max) + "_" + std::to_string(int_max),
                                          bpm_.get(), std::less<int>{}, leaf_max, int_max);
            std::vector<int> rkeys(80);
            std::iota(rkeys.begin(), rkeys.end(), 1);
            std::mt19937 rng(9999 + leaf_max * 10 + int_max);
            std::shuffle(rkeys.begin(), rkeys.end(), rng);
            for (int rk : rkeys) {
                ASSERT_TRUE(rand_tree.Insert(rk, RID(4, static_cast<uint32_t>(rk))));
                std::string err;
                ASSERT_TRUE(rand_tree.CheckIntegrity(&err))
                    << "Random fanout (" << leaf_max << "," << int_max << ") failed at " << rk << ": " << err;
            }
        }
        AssertAllPagesUnpinned();
    }
}

