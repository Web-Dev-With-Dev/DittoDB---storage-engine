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


// ============================================================================
// Test 24: Remove from Empty Tree Returns False and Leaves Zero Pin Leaks
// ============================================================================
TEST_F(BPlusTreeTest, RemoveFromEmptyTreeReturnsFalse) {
    BPlusTree<int, RID> tree("remove_empty", bpm_.get(), std::less<int>{}, 4, 3);

    EXPECT_TRUE(tree.IsEmpty());
    EXPECT_EQ(tree.GetRecordCount(), 0u);
    EXPECT_EQ(tree.GetDepth(), 0u);

    // Attempting to remove from empty tree must cleanly return false
    EXPECT_FALSE(tree.Remove(10));
    EXPECT_FALSE(tree.Delete(20));
    EXPECT_FALSE(tree.Remove(-1));

    EXPECT_TRUE(tree.IsEmpty());
    EXPECT_EQ(tree.GetRecordCount(), 0u);
    EXPECT_EQ(tree.GetDepth(), 0u);

    auto result = tree.ValidateTree();
    EXPECT_TRUE(result.is_valid) << result.error_message;
    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 25: Remove Missing Keys Leaves Tree Intact with Clean Unpins
// ============================================================================
TEST_F(BPlusTreeTest, RemoveMissingKeysLeavesTreeIntact) {
    BPlusTree<int, RID> tree("remove_missing", bpm_.get(), std::less<int>{}, 4, 3);

    std::vector<int> keys = {10, 20, 30};
    for (int k : keys) {
        ASSERT_TRUE(tree.Insert(k, RID(1, static_cast<uint32_t>(k))));
    }

    EXPECT_EQ(tree.GetRecordCount(), 3u);

    // Missing keys: lower than min, between existing keys, greater than max
    std::vector<int> missing_keys = {-100, 0, 5, 15, 25, 35, 100, 999};
    for (int mk : missing_keys) {
        EXPECT_FALSE(tree.Remove(mk)) << "Remove should return false for missing key: " << mk;
        EXPECT_FALSE(tree.Delete(mk)) << "Delete alias should return false for missing key: " << mk;
        EXPECT_EQ(tree.GetRecordCount(), 3u);
    }

    // Verify all original keys are still present and unaltered
    for (int k : keys) {
        RID r;
        EXPECT_TRUE(tree.GetValue(k, &r));
        EXPECT_EQ(r.GetPageId(), 1);
        EXPECT_EQ(r.GetSlotNum(), static_cast<uint32_t>(k));
    }

    auto result = tree.ValidateTree();
    EXPECT_TRUE(result.is_valid) << result.error_message;
    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 26: Remove from Single Leaf Root Without Underflow
// ============================================================================
TEST_F(BPlusTreeTest, RemoveFromSingleLeafRootWithoutUnderflow) {
    BPlusTree<int, RID> tree("single_leaf_remove", bpm_.get(), std::less<int>{}, 5, 4);

    // Insert 4 keys into single leaf root (max_size = 5)
    std::vector<int> keys = {10, 20, 30, 40};
    for (int k : keys) {
        ASSERT_TRUE(tree.Insert(k, RID(1, static_cast<uint32_t>(k))));
    }
    EXPECT_EQ(tree.GetRecordCount(), 4u);
    EXPECT_EQ(tree.GetDepth(), 1u);

    // 1. Remove middle key 20
    EXPECT_TRUE(tree.Remove(20));
    EXPECT_EQ(tree.GetRecordCount(), 3u);
    EXPECT_FALSE(tree.Contains(20));
    EXPECT_TRUE(tree.ValidateTree().is_valid);

    // Duplicate remove of 20 should fail
    EXPECT_FALSE(tree.Remove(20));
    EXPECT_EQ(tree.GetRecordCount(), 3u);

    // 2. Remove last key 40
    EXPECT_TRUE(tree.Remove(40));
    EXPECT_EQ(tree.GetRecordCount(), 2u);
    EXPECT_FALSE(tree.Contains(40));
    EXPECT_TRUE(tree.ValidateTree().is_valid);

    // 3. Remove first key 10
    EXPECT_TRUE(tree.Remove(10));
    EXPECT_EQ(tree.GetRecordCount(), 1u);
    EXPECT_FALSE(tree.Contains(10));

    // Verify sole remaining key 30
    RID r;
    EXPECT_TRUE(tree.GetValue(30, &r));
    EXPECT_EQ(r.GetSlotNum(), 30u);
    EXPECT_TRUE(tree.ValidateTree().is_valid);

    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 27: Remove from Single Leaf Root Down to Empty and Reinsert
// ============================================================================
TEST_F(BPlusTreeTest, RemoveSingleLeafRootDownToEmptyAndReinsert) {
    BPlusTree<int, RID> tree("root_down_to_empty", bpm_.get(), std::less<int>{}, 4, 3);

    ASSERT_TRUE(tree.Insert(42, RID(1, 42)));
    EXPECT_FALSE(tree.IsEmpty());
    EXPECT_EQ(tree.GetRecordCount(), 1u);
    EXPECT_EQ(tree.GetDepth(), 1u);

    // Remove the only key
    EXPECT_TRUE(tree.Remove(42));
    EXPECT_TRUE(tree.IsEmpty());
    EXPECT_EQ(tree.GetRecordCount(), 0u);
    EXPECT_EQ(tree.GetDepth(), 0u);
    EXPECT_EQ(tree.GetRootPageId(), INVALID_PAGE_ID);

    auto result = tree.ValidateTree();
    EXPECT_TRUE(result.is_valid) << result.error_message;

    // Verify lookup fails
    EXPECT_FALSE(tree.Contains(42));

    // Re-insert into the emptied tree
    ASSERT_TRUE(tree.Insert(100, RID(2, 100)));
    ASSERT_TRUE(tree.Insert(200, RID(2, 200)));

    EXPECT_FALSE(tree.IsEmpty());
    EXPECT_EQ(tree.GetRecordCount(), 2u);
    EXPECT_EQ(tree.GetDepth(), 1u);

    RID r;
    EXPECT_TRUE(tree.GetValue(100, &r));
    EXPECT_EQ(r.GetSlotNum(), 100u);
    EXPECT_TRUE(tree.GetValue(200, &r));
    EXPECT_EQ(r.GetSlotNum(), 200u);

    EXPECT_TRUE(tree.ValidateTree().is_valid);
    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 28: Remove from Multi-Level Tree Without Underflow
// ============================================================================
TEST_F(BPlusTreeTest, RemoveFromMultiLevelTreeWithoutUnderflow) {
    // leaf_max_size = 4, internal_max_size = 3
    // Non-root leaf min_size = 4 / 2 = 2
    BPlusTree<int, RID> tree("multilevel_no_underflow", bpm_.get(), std::less<int>{}, 4, 3);

    // Insert keys carefully to produce a multi-level tree
    std::vector<int> initial_keys = {10, 20, 30, 40, 50, 60, 70, 80};
    for (int k : initial_keys) {
        ASSERT_TRUE(tree.Insert(k, RID(1, static_cast<uint32_t>(k))));
    }

    EXPECT_GT(tree.GetDepth(), 1u);
    EXPECT_EQ(tree.GetRecordCount(), 8u);
    EXPECT_TRUE(tree.ValidateTree().is_valid);

    // Now insert key 25. This inserts into the leaf containing 20 or 30,
    // increasing that leaf's size from 2 to 3.
    ASSERT_TRUE(tree.Insert(25, RID(1, 25)));
    EXPECT_EQ(tree.GetRecordCount(), 9u);
    EXPECT_TRUE(tree.ValidateTree().is_valid);

    // Removing 25 will decrease that leaf's size from 3 back to 2.
    // Since 2 >= min_size (2), NO underflow occurs!
    EXPECT_TRUE(tree.Remove(25));
    EXPECT_EQ(tree.GetRecordCount(), 8u);
    EXPECT_FALSE(tree.Contains(25));

    // Also remove missing keys in multi-level tree
    EXPECT_FALSE(tree.Remove(25)); // already deleted
    EXPECT_FALSE(tree.Remove(999));
    EXPECT_FALSE(tree.Remove(-5));
    EXPECT_EQ(tree.GetRecordCount(), 8u);

    // Verify all 8 original keys are fully intact and correct
    for (int k : initial_keys) {
        RID r;
        EXPECT_TRUE(tree.GetValue(k, &r)) << "Key " << k << " missing after removal!";
        EXPECT_EQ(r.GetSlotNum(), static_cast<uint32_t>(k));
    }

    std::string err;
    EXPECT_TRUE(tree.CheckIntegrity(&err)) << "Integrity failed: " << err;
    EXPECT_TRUE(tree.ValidateTree().is_valid);
    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 29: Leaf Redistribution Borrowing from Left Sibling and Updating Parent Separator
// ============================================================================
TEST_F(BPlusTreeTest, RedistributeBorrowFromLeftSibling) {
    // leaf_max_size = 4 (min_size = 2), internal_max_size = 3
    BPlusTree<int, RID> tree("redis_left", bpm_.get(), std::less<int>{}, 4, 3);

    // Insert {10, 20, 30, 40, 50}
    // Splits into: Leaf 1 = {10, 20}, Leaf 2 = {30, 40, 50}, separator = 30
    for (int k : {10, 20, 30, 40, 50}) {
        ASSERT_TRUE(tree.Insert(k, RID(1, static_cast<uint32_t>(k))));
    }

    // Insert 25 into Leaf 1 -> Leaf 1 now has {10, 20, 25} (size 3 > min_size 2)
    ASSERT_TRUE(tree.Insert(25, RID(1, 25)));
    EXPECT_EQ(tree.GetRecordCount(), 6u);
    EXPECT_TRUE(tree.ValidateTree().is_valid);

    // Remove 50 from Leaf 2 -> Leaf 2 has {30, 40} (size 2 == min_size)
    ASSERT_TRUE(tree.Remove(50));
    EXPECT_EQ(tree.GetRecordCount(), 5u);
    EXPECT_TRUE(tree.ValidateTree().is_valid);

    // Remove 40 from Leaf 2 -> Leaf 2 now has {30} (size 1 < min_size 2) -> UNDERFLOW!
    // Leaf 2 borrows 25 from its left sibling Leaf 1 ({10, 20, 25})!
    // Result:
    // Leaf 1: {10, 20} (size 2)
    // Leaf 2: {25, 30} (size 2)
    // Parent separator updated from 30 to 25!
    ASSERT_TRUE(tree.Remove(40));
    EXPECT_EQ(tree.GetRecordCount(), 4u);
    EXPECT_FALSE(tree.Contains(40));
    EXPECT_FALSE(tree.Contains(50));

    // Verify all 4 remaining keys can be located
    for (int k : {10, 20, 25, 30}) {
        RID r;
        EXPECT_TRUE(tree.GetValue(k, &r)) << "Key " << k << " must be retrievable after redistribution";
        EXPECT_EQ(r.GetSlotNum(), static_cast<uint32_t>(k));
    }

    std::string err;
    EXPECT_TRUE(tree.CheckIntegrity(&err)) << "Integrity check failed: " << err;
    EXPECT_TRUE(tree.ValidateTree().is_valid);
    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 30: Leaf Redistribution Borrowing from Right Sibling and Updating Parent Separator
// ============================================================================
TEST_F(BPlusTreeTest, RedistributeBorrowFromRightSibling) {
    // leaf_max_size = 4 (min_size = 2), internal_max_size = 3
    BPlusTree<int, RID> tree("redis_right", bpm_.get(), std::less<int>{}, 4, 3);

    // Insert {10, 20, 30, 40, 50}
    // Splits into: Leaf 1 = {10, 20} (size 2 == min_size), Leaf 2 = {30, 40, 50} (size 3 > min_size), sep = 30
    for (int k : {10, 20, 30, 40, 50}) {
        ASSERT_TRUE(tree.Insert(k, RID(1, static_cast<uint32_t>(k))));
    }

    EXPECT_EQ(tree.GetRecordCount(), 5u);
    EXPECT_TRUE(tree.ValidateTree().is_valid);

    // Remove 20 from Leaf 1 -> Leaf 1 has {10} (size 1 < min_size 2) -> UNDERFLOW!
    // Leaf 1 is child 0 (no left sibling), so it borrows first entry (30) from right sibling Leaf 2!
    // Result:
    // Leaf 1: {10, 30} (size 2)
    // Leaf 2: {40, 50} (size 2)
    // Parent separator updated from 30 to 40!
    ASSERT_TRUE(tree.Remove(20));
    EXPECT_EQ(tree.GetRecordCount(), 4u);
    EXPECT_FALSE(tree.Contains(20));

    // Verify borrowed key 30 routes correctly to Leaf 1
    RID r30;
    EXPECT_TRUE(tree.GetValue(30, &r30));
    EXPECT_EQ(r30.GetSlotNum(), 30u);

    // Verify remaining keys 10, 40, 50
    for (int k : {10, 40, 50}) {
        RID r;
        EXPECT_TRUE(tree.GetValue(k, &r)) << "Key " << k << " must be retrievable after redistribution";
        EXPECT_EQ(r.GetSlotNum(), static_cast<uint32_t>(k));
    }

    std::string err;
    EXPECT_TRUE(tree.CheckIntegrity(&err)) << "Integrity check failed: " << err;
    EXPECT_TRUE(tree.ValidateTree().is_valid);
    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 31: Direct Redistribution Primitives and Sibling Boundary Checks
// ============================================================================
TEST_F(BPlusTreeTest, RedistributeDirectPrimitivesAndBoundaryChecks) {
    BPlusTree<int, RID> tree("redis_direct", bpm_.get(), std::less<int>{}, 4, 3);

    // Cannot redistribute nullptr or root page
    EXPECT_FALSE(tree.Redistribute(static_cast<BPlusTreeLeafPage<int, RID>*>(nullptr)));
    EXPECT_FALSE(tree.Redistribute(static_cast<BPlusTreeInternalPage<int, page_id_t>*>(nullptr)));

    // Insert 2 keys into root
    ASSERT_TRUE(tree.Insert(1, RID(1, 1)));
    ASSERT_TRUE(tree.Insert(2, RID(1, 2)));

    Page *root_frame = bpm_->FetchPage(tree.GetRootPageId());
    ASSERT_NE(root_frame, nullptr);
    auto *root_leaf = BPlusTreeLeafPage<int, RID>::Cast(root_frame);

    // Root leaf should not be redistributed
    EXPECT_FALSE(tree.Redistribute(root_leaf));
    bpm_->UnpinPage(tree.GetRootPageId(), false);

    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 32: Multiple Alternating Redistributions Preserving Invariants
// ============================================================================
TEST_F(BPlusTreeTest, RedistributeSequencePreservingValidation) {
    // leaf_max_size = 4, internal_max_size = 4
    BPlusTree<int, RID> tree("redis_seq", bpm_.get(), std::less<int>{}, 4, 4);

    // Insert a sequence to create multiple leaves with spare capacity
    std::vector<int> data = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120};
    for (int d : data) {
        ASSERT_TRUE(tree.Insert(d, RID(1, static_cast<uint32_t>(d))));
    }

    EXPECT_TRUE(tree.ValidateTree().is_valid);

    // Add extra keys to specific leaves
    ASSERT_TRUE(tree.Insert(15, RID(1, 15)));
    ASSERT_TRUE(tree.Insert(35, RID(1, 35)));
    ASSERT_TRUE(tree.Insert(55, RID(1, 55)));
    ASSERT_TRUE(tree.Insert(75, RID(1, 75)));

    EXPECT_TRUE(tree.ValidateTree().is_valid);

    // Delete keys that trigger redistribution without needing full merge
    for (int del_key : {10, 30, 50, 70}) {
        EXPECT_TRUE(tree.Remove(del_key)) << "Failed to remove " << del_key;
        EXPECT_FALSE(tree.Contains(del_key));

        std::string err;
        EXPECT_TRUE(tree.CheckIntegrity(&err)) << "Integrity failed after removing " << del_key << ": " << err;
        auto result = tree.ValidateTree();
        EXPECT_TRUE(result.is_valid) << "Validation failed after removing " << del_key << ": " << result.error_message;
    }

    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 33: Leaf Coalesce with Left Sibling, Parent Entry Deletion, and Root Demotion
// ============================================================================
TEST_F(BPlusTreeTest, CoalesceLeftSiblingFixLeafChainAndFreePage) {
    // leaf_max_size = 4 (min_size = 2), internal_max_size = 3
    BPlusTree<int, RID> tree("coalesce_left", bpm_.get(), std::less<int>{}, 4, 3);

    // Insert {10, 20, 30, 40} -> fills single leaf
    // Insert 50 -> splits into Leaf 1 {10, 20} and Leaf 2 {30, 40, 50}, internal root
    for (int k : {10, 20, 30, 40, 50}) {
        ASSERT_TRUE(tree.Insert(k, RID(1, static_cast<uint32_t>(k))));
    }

    EXPECT_EQ(tree.GetDepth(), 2u);
    EXPECT_EQ(tree.GetRecordCount(), 5u);

    // Remove 50 -> Leaf 2 has {30, 40} (size 2 == min_size)
    ASSERT_TRUE(tree.Remove(50));
    EXPECT_EQ(tree.GetRecordCount(), 4u);
    EXPECT_EQ(tree.GetDepth(), 2u);
    EXPECT_TRUE(tree.ValidateTree().is_valid);

    // Both Leaf 1 {10, 20} and Leaf 2 {30, 40} are at min_size (2).
    // Now remove 40 from Leaf 2:
    // - Leaf 2 underflows to size 1.
    // - Sibling Leaf 1 has size 2 (cannot spare an entry).
    // - Leaf Coalesce merges Leaf 2 into Leaf 1!
    // - Leaf 1 gets 30, size becomes 3: {10, 20, 30}.
    // - Leaf chain: Leaf 1 next_page_id updated to Leaf 2's next_page_id (INVALID_PAGE_ID).
    // - Parent deletes entry pointing to Leaf 2.
    // - Internal root shrinks to 1 child -> Leaf 1 is promoted to root!
    // - Old internal root and Leaf 2 pages are freed in BPM.
    // - Depth becomes 1!
    ASSERT_TRUE(tree.Remove(40));
    EXPECT_EQ(tree.GetRecordCount(), 3u);
    EXPECT_EQ(tree.GetDepth(), 1u);
    EXPECT_FALSE(tree.Contains(40));
    EXPECT_FALSE(tree.Contains(50));

    // Verify remaining keys
    for (int k : {10, 20, 30}) {
        RID r;
        EXPECT_TRUE(tree.GetValue(k, &r));
        EXPECT_EQ(r.GetSlotNum(), static_cast<uint32_t>(k));
    }

    std::string err;
    EXPECT_TRUE(tree.CheckIntegrity(&err)) << "Integrity failed: " << err;
    EXPECT_TRUE(tree.ValidateTree().is_valid);
    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 34: Leaf Coalesce with Right Sibling (Child 0 Underflows)
// ============================================================================
TEST_F(BPlusTreeTest, CoalesceRightSiblingFixLeafChainAndFreePage) {
    // leaf_max_size = 4 (min_size = 2), internal_max_size = 3
    BPlusTree<int, RID> tree("coalesce_right", bpm_.get(), std::less<int>{}, 4, 3);

    for (int k : {10, 20, 30, 40, 50}) {
        ASSERT_TRUE(tree.Insert(k, RID(1, static_cast<uint32_t>(k))));
    }
    // Remove 50 so both leaves have size 2: Leaf 1 {10, 20}, Leaf 2 {30, 40}
    ASSERT_TRUE(tree.Remove(50));
    EXPECT_EQ(tree.GetDepth(), 2u);

    // Remove 10 from Leaf 1 (child 0):
    // - Leaf 1 underflows to size 1 {20}.
    // - No left sibling. Right sibling Leaf 2 has size 2 (cannot spare).
    // - Coalesce merges Leaf 2 into Leaf 1!
    // - Leaf 1 gets {30, 40}, size becomes 3: {20, 30, 40}.
    // - Leaf chain: Leaf 1 next_page_id updated to INVALID_PAGE_ID.
    // - Parent removes entry for Leaf 2.
    // - Root demotes to Leaf 1 (depth becomes 1).
    // - Leaf 2 and old internal root freed.
    ASSERT_TRUE(tree.Remove(10));
    EXPECT_EQ(tree.GetRecordCount(), 3u);
    EXPECT_EQ(tree.GetDepth(), 1u);
    EXPECT_FALSE(tree.Contains(10));

    for (int k : {20, 30, 40}) {
        RID r;
        EXPECT_TRUE(tree.GetValue(k, &r));
        EXPECT_EQ(r.GetSlotNum(), static_cast<uint32_t>(k));
    }

    std::string err;
    EXPECT_TRUE(tree.CheckIntegrity(&err)) << "Integrity failed: " << err;
    EXPECT_TRUE(tree.ValidateTree().is_valid);
    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 35: Leaf Coalesce in Tree with Three Leaves (Parent Retains Size 2)
// ============================================================================
TEST_F(BPlusTreeTest, CoalesceInTreeWithThreeLeavesParentRetainsInternal) {
    // leaf_max_size = 4 (min_size = 2), internal_max_size = 4
    BPlusTree<int, RID> tree("coalesce_three_leaves", bpm_.get(), std::less<int>{}, 4, 4);

    // Insert to form 3 leaves:
    // Leaf 1: {10, 20}
    // Leaf 2: {30, 40}
    // Leaf 3: {50, 60, 70}
    for (int k : {10, 20, 30, 40, 50, 60, 70}) {
        ASSERT_TRUE(tree.Insert(k, RID(1, static_cast<uint32_t>(k))));
    }
    // Remove 70 so Leaf 3 has size 2: {50, 60}
    ASSERT_TRUE(tree.Remove(70));
    EXPECT_EQ(tree.GetDepth(), 2u);
    EXPECT_EQ(tree.GetRecordCount(), 6u);
    EXPECT_TRUE(tree.ValidateTree().is_valid);

    // All 3 leaves are at min_size (2).
    // Now remove 40 from Leaf 2 (middle child):
    // - Leaf 2 underflows to size 1 {30}.
    // - Left sibling Leaf 1 has size 2 (cannot spare).
    // - Coalesce merges Leaf 2 into Leaf 1!
    // - Leaf 1 gets 30 -> {10, 20, 30}.
    // - Leaf chain bridged: Leaf 1 -> Leaf 3! (Leaf 2 bypassed).
    // - In parent, entry for Leaf 2 deleted.
    // - Parent still has 2 children (Leaf 1 and Leaf 3) -> parent remains valid internal root!
    // - Depth remains 2!
    // - Leaf 2 page is freed.
    ASSERT_TRUE(tree.Remove(40));
    EXPECT_EQ(tree.GetRecordCount(), 5u);
    EXPECT_EQ(tree.GetDepth(), 2u);
    EXPECT_FALSE(tree.Contains(40));

    // Verify all remaining keys in both leaves
    for (int k : {10, 20, 30, 50, 60}) {
        RID r;
        EXPECT_TRUE(tree.GetValue(k, &r));
        EXPECT_EQ(r.GetSlotNum(), static_cast<uint32_t>(k));
    }

    std::string err;
    EXPECT_TRUE(tree.CheckIntegrity(&err)) << "Integrity failed: " << err;
    auto val_res = tree.ValidateTree();
    EXPECT_TRUE(val_res.is_valid) << val_res.error_message;
    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 36: Continuous Coalescing from Multi-Level Down to Empty and Growth
// ============================================================================
TEST_F(BPlusTreeTest, ContinuousCoalescingDownToEmptyAndRegrowth) {
    // leaf_max_size = 4, internal_max_size = 3
    BPlusTree<int, RID> tree("coalesce_to_empty", bpm_.get(), std::less<int>{}, 4, 3);

    // Insert 5 keys to create a 2-level tree (internal root + 2 leaves)
    std::vector<int> data = {1, 2, 3, 4, 5};
    for (int d : data) {
        ASSERT_TRUE(tree.Insert(d, RID(1, static_cast<uint32_t>(d))));
    }
    EXPECT_EQ(tree.GetDepth(), 2u);
    EXPECT_TRUE(tree.ValidateTree().is_valid);

    // Continuously delete all keys one by one in reverse order
    for (auto it = data.rbegin(); it != data.rend(); ++it) {
        EXPECT_TRUE(tree.Remove(*it)) << "Failed removing " << *it;
        EXPECT_FALSE(tree.Contains(*it));

        auto result = tree.ValidateTree();
        EXPECT_TRUE(result.is_valid) << "Failed validation after removing " << *it << ": " << result.error_message;
    }

    // Tree should now be completely empty
    EXPECT_TRUE(tree.IsEmpty());
    EXPECT_EQ(tree.GetRecordCount(), 0u);
    EXPECT_EQ(tree.GetDepth(), 0u);
    EXPECT_EQ(tree.GetRootPageId(), INVALID_PAGE_ID);

    // Re-grow tree
    for (int k = 100; k <= 105; ++k) {
        ASSERT_TRUE(tree.Insert(k, RID(2, static_cast<uint32_t>(k))));
    }
    EXPECT_EQ(tree.GetRecordCount(), 6u);
    EXPECT_TRUE(tree.ValidateTree().is_valid);

    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 37: Deep Multi-Level Tree Shrink: Internal Coalesce and Root Collapse
// ============================================================================
TEST_F(BPlusTreeTest, InternalCoalesceAndRootCollapseDepthRegression) {
    // leaf_max_size = 4, internal_max_size = 3
    // With internal_max_size = 3, min_internal = (3 + 1) / 2 = 2
    BPlusTree<int, RID> tree("deep_shrink", bpm_.get(), std::less<int>{}, 4, 3);

    // Insert keys 1 .. 18 to grow tree to depth 3
    std::vector<int> keys;
    for (int i = 1; i <= 18; ++i) {
        keys.push_back(i);
        ASSERT_TRUE(tree.Insert(i, RID(1, static_cast<uint32_t>(i))));
    }

    EXPECT_GE(tree.GetDepth(), 3u);
    EXPECT_EQ(tree.GetRecordCount(), 18u);
    EXPECT_TRUE(tree.ValidateTree().is_valid);

    // Remove keys sequentially: causes leaf coalesces, which trigger internal
    // underflows, internal coalesces, and root collapses (depth 3 -> 2 -> 1 -> 0)
    for (int k : keys) {
        EXPECT_TRUE(tree.Remove(k)) << "Failed removing key " << k;
        EXPECT_FALSE(tree.Contains(k));

        std::string err;
        EXPECT_TRUE(tree.CheckIntegrity(&err)) << "Integrity failed after removing " << k << ": " << err;
        auto val = tree.ValidateTree();
        EXPECT_TRUE(val.is_valid) << "Validation failed after removing " << k << ": " << val.error_message;
    }

    // Fully collapsed to empty tree
    EXPECT_TRUE(tree.IsEmpty());
    EXPECT_EQ(tree.GetRecordCount(), 0u);
    EXPECT_EQ(tree.GetDepth(), 0u);
    EXPECT_EQ(tree.GetRootPageId(), INVALID_PAGE_ID);

    // Re-insert into the fully collapsed tree
    for (int i = 50; i <= 60; ++i) {
        ASSERT_TRUE(tree.Insert(i, RID(2, static_cast<uint32_t>(i))));
    }
    EXPECT_EQ(tree.GetRecordCount(), 11u);
    EXPECT_TRUE(tree.ValidateTree().is_valid);
    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 38: Random Removals with Continuous Validation (Internal Redistribute & Coalesce)
// ============================================================================
TEST_F(BPlusTreeTest, RandomRemovalsTriggeringInternalRedistributeAndCoalesce) {
    // leaf_max_size = 4, internal_max_size = 3
    BPlusTree<int, RID> tree("random_removals", bpm_.get(), std::less<int>{}, 4, 3);

    std::vector<int> keys(30);
    std::iota(keys.begin(), keys.end(), 1);
    std::mt19937 rng(424242);
    std::shuffle(keys.begin(), keys.end(), rng);

    for (int k : keys) {
        ASSERT_TRUE(tree.Insert(k, RID(1, static_cast<uint32_t>(k))));
    }
    EXPECT_GE(tree.GetDepth(), 3u);
    EXPECT_TRUE(tree.ValidateTree().is_valid);

    // Shuffle for random deletion order
    std::shuffle(keys.begin(), keys.end(), rng);

    for (size_t i = 0; i < keys.size(); ++i) {
        int k = keys[i];
        EXPECT_TRUE(tree.Remove(k)) << "Failed removing " << k;
        EXPECT_FALSE(tree.Contains(k));

        std::string err;
        EXPECT_TRUE(tree.CheckIntegrity(&err)) << "Integrity failed after step " << i << " (key " << k << "): " << err;
        auto res = tree.ValidateTree();
        EXPECT_TRUE(res.is_valid) << "Validation failed after step " << i << " (key " << k << "): " << res.error_message;
    }

    EXPECT_TRUE(tree.IsEmpty());
    EXPECT_EQ(tree.GetRecordCount(), 0u);
    EXPECT_EQ(tree.GetDepth(), 0u);
    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 39: Direct Internal Page Redistribution Primitives
// ============================================================================
TEST_F(BPlusTreeTest, DirectInternalPageRedistributionPrimitives) {
    // Directly test internal page MoveAllTo, MoveLastToFrontOf, MoveFirstToEndOf
    page_id_t p1_id = INVALID_PAGE_ID;
    page_id_t p2_id = INVALID_PAGE_ID;

    Page *p1_frame = bpm_->NewPage(&p1_id);
    Page *p2_frame = bpm_->NewPage(&p2_id);
    ASSERT_NE(p1_frame, nullptr);
    ASSERT_NE(p2_frame, nullptr);

    auto *p1 = BPlusTreeInternalPage<int, page_id_t>::Cast(p1_frame);
    auto *p2 = BPlusTreeInternalPage<int, page_id_t>::Cast(p2_frame);
    p1->Init(p1_id, INVALID_PAGE_ID, 4);
    p2->Init(p2_id, INVALID_PAGE_ID, 4);

    // Setup p1 with 3 children: [100, 101, 102], keys [0, 10, 20]
    p1->SetValueAt(0, 100);
    p1->SetKeyAt(1, 10);
    p1->SetValueAt(1, 101);
    p1->SetKeyAt(2, 20);
    p1->SetValueAt(2, 102);
    p1->SetSize(3);

    // Setup p2 with 1 child: [103]
    p2->SetValueAt(0, 103);
    p2->SetSize(1);

    // MoveLastToFrontOf: p1 lends last child (102) to p2 with middle_key 30
    p1->MoveLastToFrontOf(p2, 30, bpm_.get());
    EXPECT_EQ(p1->GetSize(), 2);
    EXPECT_EQ(p2->GetSize(), 2);
    EXPECT_EQ(p2->ValueAt(0), 102);
    EXPECT_EQ(p2->KeyAt(1), 30);
    EXPECT_EQ(p2->ValueAt(1), 103);

    // MoveFirstToEndOf: p2 lends first child (102) back to p1 with middle_key 25
    p2->MoveFirstToEndOf(p1, 25, bpm_.get());
    EXPECT_EQ(p1->GetSize(), 3);
    EXPECT_EQ(p2->GetSize(), 1);
    EXPECT_EQ(p1->KeyAt(2), 25);
    EXPECT_EQ(p1->ValueAt(2), 102);
    EXPECT_EQ(p2->ValueAt(0), 103);

    // MoveAllTo: merge p2 into p1 with middle_key 40
    p2->MoveAllTo(p1, 40, bpm_.get());
    EXPECT_EQ(p1->GetSize(), 4);
    EXPECT_EQ(p2->GetSize(), 0);
    EXPECT_EQ(p1->KeyAt(3), 40);
    EXPECT_EQ(p1->ValueAt(3), 103);

    bpm_->UnpinPage(p1_id, true);
    bpm_->UnpinPage(p2_id, true);
    bpm_->DeletePage(p1_id);
    bpm_->DeletePage(p2_id);
    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 40: Workload 1 - Sequential Insert & Sequential Delete (Validate Every Op)
// ============================================================================
TEST_F(BPlusTreeTest, WorkloadSequentialInsertDeleteValidateEveryOp) {
    // Small fanouts: leaf_max = 4, internal_max = 3 (forces splits, redistributions, and merges)
    BPlusTree<int, RID> tree("workload_seq", bpm_.get(), std::less<int>{}, 4, 3);

    const int total_keys = 35;

    // 1. Sequential Insertions (1 .. total_keys)
    for (int i = 1; i <= total_keys; ++i) {
        ASSERT_TRUE(tree.Insert(i, RID(10, static_cast<uint32_t>(i))))
            << "Insert failed at key " << i;
        EXPECT_EQ(tree.GetRecordCount(), static_cast<uint64_t>(i));

        // Validate tree invariants after EVERY insert
        std::string err;
        ASSERT_TRUE(tree.CheckIntegrity(&err))
            << "Integrity violation during sequential insert at key " << i << ": " << err;
        auto val = tree.ValidateTree();
        ASSERT_TRUE(val.is_valid)
            << "Tree validation failure during sequential insert at key " << i << ": " << val.error_message;
    }

    // 2. Sequential Deletions (1 .. total_keys)
    for (int i = 1; i <= total_keys; ++i) {
        ASSERT_TRUE(tree.Remove(i))
            << "Remove failed at key " << i;
        EXPECT_FALSE(tree.Contains(i))
            << "Key " << i << " still found after removal!";
        EXPECT_EQ(tree.GetRecordCount(), static_cast<uint64_t>(total_keys - i));

        // Validate tree invariants after EVERY delete
        std::string err;
        ASSERT_TRUE(tree.CheckIntegrity(&err))
            << "Integrity violation during sequential delete at key " << i << ": " << err;
        auto val = tree.ValidateTree();
        ASSERT_TRUE(val.is_valid)
            << "Tree validation failure during sequential delete at key " << i << ": " << val.error_message;

        // Verify all remaining keys (i + 1 .. total_keys) can be looked up
        for (int rem = i + 1; rem <= std::min(total_keys, i + 5); ++rem) {
            RID r;
            EXPECT_TRUE(tree.GetValue(rem, &r));
            EXPECT_EQ(r.GetSlotNum(), static_cast<uint32_t>(rem));
        }
    }

    // Verify completely empty tree state
    EXPECT_TRUE(tree.IsEmpty());
    EXPECT_EQ(tree.GetRecordCount(), 0u);
    EXPECT_EQ(tree.GetDepth(), 0u);
    EXPECT_EQ(tree.GetRootPageId(), INVALID_PAGE_ID);

    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 41: Workload 2 - Reverse Insert & Reverse Delete (Validate Every Op)
// ============================================================================
TEST_F(BPlusTreeTest, WorkloadReverseInsertDeleteValidateEveryOp) {
    BPlusTree<int, RID> tree("workload_rev", bpm_.get(), std::less<int>{}, 4, 3);

    const int total_keys = 35;

    // 1. Reverse Insertions (total_keys down to 1)
    for (int i = total_keys; i >= 1; --i) {
        ASSERT_TRUE(tree.Insert(i, RID(20, static_cast<uint32_t>(i))))
            << "Reverse insert failed at key " << i;

        std::string err;
        ASSERT_TRUE(tree.CheckIntegrity(&err))
            << "Integrity violation during reverse insert at key " << i << ": " << err;
        auto val = tree.ValidateTree();
        ASSERT_TRUE(val.is_valid)
            << "Tree validation failure during reverse insert at key " << i << ": " << val.error_message;
    }

    EXPECT_EQ(tree.GetRecordCount(), static_cast<uint64_t>(total_keys));

    // 2. Reverse Deletions (total_keys down to 1)
    for (int i = total_keys; i >= 1; --i) {
        ASSERT_TRUE(tree.Remove(i))
            << "Reverse remove failed at key " << i;
        EXPECT_FALSE(tree.Contains(i));

        std::string err;
        ASSERT_TRUE(tree.CheckIntegrity(&err))
            << "Integrity violation during reverse delete at key " << i << ": " << err;
        auto val = tree.ValidateTree();
        ASSERT_TRUE(val.is_valid)
            << "Tree validation failure during reverse delete at key " << i << ": " << val.error_message;

        // Verify remaining lower keys can still be looked up
        for (int rem = std::max(1, i - 5); rem < i; ++rem) {
            RID r;
            EXPECT_TRUE(tree.GetValue(rem, &r));
            EXPECT_EQ(r.GetSlotNum(), static_cast<uint32_t>(rem));
        }
    }

    EXPECT_TRUE(tree.IsEmpty());
    EXPECT_EQ(tree.GetRecordCount(), 0u);
    EXPECT_EQ(tree.GetDepth(), 0u);
    EXPECT_EQ(tree.GetRootPageId(), INVALID_PAGE_ID);

    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 42: Workload 3 - Random Insert & Random Delete (Validate Every Op)
// ============================================================================
TEST_F(BPlusTreeTest, WorkloadRandomInsertDeleteValidateEveryOp) {
    // Minimum fanout: (3, 3) for maximum structural churn
    BPlusTree<int, RID> tree("workload_rand", bpm_.get(), std::less<int>{}, 3, 3);

    const int total_keys = 40;
    std::vector<int> keys(total_keys);
    std::iota(keys.begin(), keys.end(), 1);

    std::mt19937 rng(133742);
    std::shuffle(keys.begin(), keys.end(), rng);

    // 1. Random Insertions
    for (size_t step = 0; step < keys.size(); ++step) {
        int k = keys[step];
        ASSERT_TRUE(tree.Insert(k, RID(30, static_cast<uint32_t>(k))))
            << "Random insert failed for key " << k;
        EXPECT_EQ(tree.GetRecordCount(), step + 1);

        std::string err;
        ASSERT_TRUE(tree.CheckIntegrity(&err))
            << "Integrity violation during random insert of key " << k << ": " << err;
        auto val = tree.ValidateTree();
        ASSERT_TRUE(val.is_valid)
            << "Validation failure during random insert of key " << k << ": " << val.error_message;
    }

    // Shuffle for a completely different deletion sequence
    std::shuffle(keys.begin(), keys.end(), rng);

    // 2. Random Deletions
    for (size_t step = 0; step < keys.size(); ++step) {
        int k = keys[step];
        ASSERT_TRUE(tree.Remove(k))
            << "Random remove failed for key " << k;
        EXPECT_FALSE(tree.Contains(k));
        EXPECT_EQ(tree.GetRecordCount(), keys.size() - step - 1);

        std::string err;
        ASSERT_TRUE(tree.CheckIntegrity(&err))
            << "Integrity violation during random delete of key " << k << ": " << err;
        auto val = tree.ValidateTree();
        ASSERT_TRUE(val.is_valid)
            << "Validation failure during random delete of key " << k << ": " << val.error_message;
    }

    EXPECT_TRUE(tree.IsEmpty());
    EXPECT_EQ(tree.GetRecordCount(), 0u);
    EXPECT_EQ(tree.GetDepth(), 0u);

    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 43: Workload 4 - Interleaved Mixed Operations (Validate Every Op)
// ============================================================================
TEST_F(BPlusTreeTest, WorkloadMixedOperationsValidateEveryOp) {
    // Fanout: (4, 3)
    BPlusTree<int, RID> tree("workload_mixed", bpm_.get(), std::less<int>{}, 4, 3);

    std::set<int> oracle;
    std::mt19937 rng(88888);
    std::uniform_int_distribution<int> key_dist(1, 200);
    std::uniform_int_distribution<int> op_dist(0, 99);

    const int total_ops = 120;

    for (int op = 0; op < total_ops; ++op) {
        int choice = op_dist(rng);

        if (choice < 45) {
            // Op 1: Insert fresh key
            int k = key_dist(rng);
            if (oracle.count(k) == 0) {
                ASSERT_TRUE(tree.Insert(k, RID(40, static_cast<uint32_t>(k))));
                oracle.insert(k);
            } else {
                // Duplicate key insert must return false
                EXPECT_FALSE(tree.Insert(k, RID(40, static_cast<uint32_t>(k))));
            }
        } else if (choice < 80) {
            // Op 2: Delete existing key (if oracle non-empty)
            if (!oracle.empty()) {
                // Select an existing key from oracle
                auto it = oracle.begin();
                std::advance(it, std::uniform_int_distribution<size_t>(0, oracle.size() - 1)(rng));
                int k = *it;

                ASSERT_TRUE(tree.Remove(k)) << "Failed removing existing key " << k;
                EXPECT_FALSE(tree.Contains(k));
                oracle.erase(it);
            } else {
                // Delete missing key on empty tree
                EXPECT_FALSE(tree.Remove(key_dist(rng)));
            }
        } else if (choice < 90) {
            // Op 3: Duplicate key rejection test
            if (!oracle.empty()) {
                auto it = oracle.begin();
                std::advance(it, std::uniform_int_distribution<size_t>(0, oracle.size() - 1)(rng));
                int k = *it;
                EXPECT_FALSE(tree.Insert(k, RID(40, 9999))); // must reject
            }
        } else {
            // Op 4: Missing key removal test
            int k = key_dist(rng);
            while (oracle.count(k) > 0) {
                k = key_dist(rng);
            }
            EXPECT_FALSE(tree.Remove(k)); // must return false
        }

        // Validate state against oracle after EVERY operation
        EXPECT_EQ(tree.GetRecordCount(), oracle.size());
        EXPECT_EQ(tree.IsEmpty(), oracle.empty());

        std::string err;
        ASSERT_TRUE(tree.CheckIntegrity(&err))
            << "Integrity violation at mixed step " << op << ": " << err;
        auto val = tree.ValidateTree();
        ASSERT_TRUE(val.is_valid)
            << "Validation failure at mixed step " << op << ": " << val.error_message;
    }

    // Drain all remaining keys in oracle one by one
    while (!oracle.empty()) {
        int k = *oracle.begin();
        oracle.erase(oracle.begin());

        ASSERT_TRUE(tree.Remove(k));
        EXPECT_FALSE(tree.Contains(k));
        EXPECT_EQ(tree.GetRecordCount(), oracle.size());

        std::string err;
        ASSERT_TRUE(tree.CheckIntegrity(&err));
        auto val = tree.ValidateTree();
        ASSERT_TRUE(val.is_valid);
    }

    EXPECT_TRUE(tree.IsEmpty());
    EXPECT_EQ(tree.GetRecordCount(), 0u);
    EXPECT_EQ(tree.GetDepth(), 0u);

    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 44: Workload 5 - Multi-Fanout Workload Matrix (Validate Every Op)
// ============================================================================
TEST_F(BPlusTreeTest, WorkloadMultiFanoutMatrixStress) {
    const std::vector<std::pair<int, int>> fanouts = {
        {3, 3}, {3, 4}, {4, 3}, {4, 4}, {5, 4}
    };

    for (const auto &[leaf_max, int_max] : fanouts) {
        std::string tree_name = "matrix_" + std::to_string(leaf_max) + "_" + std::to_string(int_max);
        BPlusTree<int, RID> tree(tree_name, bpm_.get(), std::less<int>{}, leaf_max, int_max);

        const int num_keys = 24;

        // Sequential insert
        for (int i = 1; i <= num_keys; ++i) {
            ASSERT_TRUE(tree.Insert(i, RID(50, static_cast<uint32_t>(i))));
            std::string err;
            ASSERT_TRUE(tree.CheckIntegrity(&err))
                << "Fanout (" << leaf_max << "," << int_max << ") insert integrity failed at " << i << ": " << err;
            auto val = tree.ValidateTree();
            ASSERT_TRUE(val.is_valid)
                << "Fanout (" << leaf_max << "," << int_max << ") insert validation failed at " << i << ": " << val.error_message;
        }

        // Random delete
        std::vector<int> del_order(num_keys);
        std::iota(del_order.begin(), del_order.end(), 1);
        std::mt19937 rng(42 + leaf_max * 10 + int_max);
        std::shuffle(del_order.begin(), del_order.end(), rng);

        for (int k : del_order) {
            ASSERT_TRUE(tree.Remove(k));
            EXPECT_FALSE(tree.Contains(k));
            std::string err;
            ASSERT_TRUE(tree.CheckIntegrity(&err))
                << "Fanout (" << leaf_max << "," << int_max << ") delete integrity failed at " << k << ": " << err;
            auto val = tree.ValidateTree();
            ASSERT_TRUE(val.is_valid)
                << "Fanout (" << leaf_max << "," << int_max << ") delete validation failed at " << k << ": " << val.error_message;
        }

        EXPECT_TRUE(tree.IsEmpty());
        EXPECT_EQ(tree.GetRecordCount(), 0u);
        EXPECT_EQ(tree.GetDepth(), 0u);

        AssertAllPagesUnpinned();
    }
}

// ============================================================================
// Test 45: Multi-Level Cascaded Root Collapse Edge Case
// ============================================================================
TEST_F(BPlusTreeTest, MultiLevelCascadedRootCollapse) {
    // Small fanouts to force deep tree (depth >= 4) with 100 keys
    BPlusTree<int, RID> tree("cascaded_collapse", bpm_.get(), std::less<int>{}, 3, 3);

    for (int i = 1; i <= 80; ++i) {
        ASSERT_TRUE(tree.Insert(i, RID(10, static_cast<uint32_t>(i))));
    }

    uint32_t initial_depth = tree.GetDepth();
    EXPECT_GE(initial_depth, 3u);
    EXPECT_TRUE(tree.ValidateTree().is_valid);

    // Delete keys in order: verifies that multiple levels of internal root collapses
    // unfold cleanly down to 0 without hanging, violating internal size bounds, or leaking pins.
    for (int i = 1; i <= 80; ++i) {
        ASSERT_TRUE(tree.Remove(i));
        EXPECT_FALSE(tree.Contains(i));
        auto val = tree.ValidateTree();
        ASSERT_TRUE(val.is_valid) << "Failed validation after removing " << i << ": " << val.error_message;
    }

    EXPECT_TRUE(tree.IsEmpty());
    EXPECT_EQ(tree.GetRecordCount(), 0u);
    EXPECT_EQ(tree.GetDepth(), 0u);
    EXPECT_EQ(tree.GetRootPageId(), INVALID_PAGE_ID);
    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 46: Deleting Smallest Key in Subtree Without Underflow
// ============================================================================
TEST_F(BPlusTreeTest, DeletingSmallestKeyInSubtreeWithoutUnderflow) {
    // Leaf max 4 (min 2), internal max 4 (min 2)
    BPlusTree<int, RID> tree("delete_smallest_no_underflow", bpm_.get(), std::less<int>{}, 4, 4);

    // Populate with keys 10, 20, 30, 40, 50, 60, 70, 80
    for (int k : {10, 20, 30, 40, 50, 60, 70, 80}) {
        ASSERT_TRUE(tree.Insert(k, RID(1, static_cast<uint32_t>(k))));
    }

    // Leaf containing {50, 60, 70} or similar has 50 as router separator in internal ancestor.
    // Removing 50 removes the first key from that leaf, leaving size >= min_size (no underflow).
    ASSERT_TRUE(tree.Remove(50));
    EXPECT_FALSE(tree.Contains(50));
    EXPECT_TRUE(tree.ValidateTree().is_valid);

    // Ensure all other keys are still retrievable
    for (int k : {10, 20, 30, 40, 60, 70, 80}) {
        RID r;
        EXPECT_TRUE(tree.GetValue(k, &r));
        EXPECT_EQ(r.GetSlotNum(), static_cast<uint32_t>(k));
    }

    // Now remove 10 (smallest key in entire tree, no underflow)
    ASSERT_TRUE(tree.Remove(10));
    EXPECT_FALSE(tree.Contains(10));
    EXPECT_TRUE(tree.ValidateTree().is_valid);

    for (int k : {20, 30, 40, 60, 70, 80}) {
        RID r;
        EXPECT_TRUE(tree.GetValue(k, &r));
        EXPECT_EQ(r.GetSlotNum(), static_cast<uint32_t>(k));
    }

    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 47: Exhaustive Single Key Insert-Delete Cycling
// ============================================================================
TEST_F(BPlusTreeTest, ExhaustiveSingleKeyInsertDeleteCycling) {
    BPlusTree<int, RID> tree("single_key_cycling", bpm_.get(), std::less<int>{}, 3, 3);

    for (int round = 1; round <= 20; ++round) {
        int key = round * 100;
        ASSERT_TRUE(tree.Insert(key, RID(1, static_cast<uint32_t>(key))));
        EXPECT_FALSE(tree.IsEmpty());
        EXPECT_EQ(tree.GetRecordCount(), 1u);
        EXPECT_EQ(tree.GetDepth(), 1u);
        EXPECT_NE(tree.GetRootPageId(), INVALID_PAGE_ID);
        EXPECT_TRUE(tree.ValidateTree().is_valid);

        RID val;
        EXPECT_TRUE(tree.GetValue(key, &val));
        EXPECT_EQ(val.GetSlotNum(), static_cast<uint32_t>(key));

        ASSERT_TRUE(tree.Remove(key));
        EXPECT_TRUE(tree.IsEmpty());
        EXPECT_EQ(tree.GetRecordCount(), 0u);
        EXPECT_EQ(tree.GetDepth(), 0u);
        EXPECT_EQ(tree.GetRootPageId(), INVALID_PAGE_ID);
        EXPECT_TRUE(tree.ValidateTree().is_valid);

        AssertAllPagesUnpinned();
    }
}

// ============================================================================
// Test 48: Coalesce Fallback to Right Sibling When Left Sibling Cannot Merge
// ============================================================================
TEST_F(BPlusTreeTest, CoalesceFallbackToRightSibling) {
    // leaf_max_size = 4, internal_max_size = 4
    BPlusTree<int, RID> tree("fallback_right", bpm_.get(), std::less<int>{}, 4, 4);

    // Build a multi-leaf tree
    for (int i = 1; i <= 15; ++i) {
        ASSERT_TRUE(tree.Insert(i, RID(1, static_cast<uint32_t>(i))));
    }
    EXPECT_TRUE(tree.ValidateTree().is_valid);

    // Delete arbitrary keys in middle leaves to exercise fallback paths
    for (int i : {7, 8, 9, 4, 5, 6}) {
        ASSERT_TRUE(tree.Remove(i));
        auto val = tree.ValidateTree();
        ASSERT_TRUE(val.is_valid) << "Failed validation at " << i << ": " << val.error_message;
    }

    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 49: IndexIterator on Empty Tree
// ============================================================================
TEST_F(BPlusTreeTest, IndexIteratorOnEmptyTree) {
    BPlusTree<int, RID> tree("iter_empty", bpm_.get(), std::less<int>{}, 4, 3);

    auto it = tree.Begin();
    auto end = tree.End();

    EXPECT_TRUE(it == end);
    EXPECT_FALSE(it != end);
    EXPECT_TRUE(it.IsEnd());
    EXPECT_FALSE(static_cast<bool>(it));

    // Begin(key) on empty tree also returns End()
    auto it_key = tree.Begin(42);
    EXPECT_TRUE(it_key == end);

    // Range-for loop should execute 0 times
    int count = 0;
    for (const auto &pair : tree) {
        (void)pair;
        count++;
    }
    EXPECT_EQ(count, 0);

    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 50: IndexIterator on Single Leaf Page
// ============================================================================
TEST_F(BPlusTreeTest, IndexIteratorSingleLeafTraversal) {
    BPlusTree<int, RID> tree("iter_single_leaf", bpm_.get(), std::less<int>{}, 5, 3);

    std::vector<int> keys = {10, 20, 30, 40};
    for (int k : keys) {
        ASSERT_TRUE(tree.Insert(k, RID(1, static_cast<uint32_t>(k))));
    }

    std::vector<int> scanned_keys;
    std::vector<uint32_t> scanned_slots;

    for (auto it = tree.Begin(); it != tree.End(); ++it) {
        EXPECT_FALSE(it.IsEnd());
        EXPECT_TRUE(static_cast<bool>(it));
        scanned_keys.push_back(it->first);
        scanned_slots.push_back(it->second.GetSlotNum());
        EXPECT_EQ(it.Key(), it->first);
        EXPECT_EQ(it.Value().GetSlotNum(), it->second.GetSlotNum());
        EXPECT_EQ((*it).first, it->first);
    }

    EXPECT_EQ(scanned_keys, keys);
    EXPECT_EQ(scanned_slots, (std::vector<uint32_t>{10, 20, 30, 40}));

    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 51: IndexIterator Across Multi-Level Leaf Chain
// ============================================================================
TEST_F(BPlusTreeTest, IndexIteratorMultiLevelChainSequentialScan) {
    // Small fanout to force deep tree with many splits and leaf chain links
    BPlusTree<int, RID> tree("iter_multilevel", bpm_.get(), std::less<int>{}, 3, 3);

    const int total_keys = 60;
    for (int i = 1; i <= total_keys; ++i) {
        ASSERT_TRUE(tree.Insert(i, RID(10, static_cast<uint32_t>(i))));
    }

    EXPECT_GT(tree.GetDepth(), 2u);

    // Sequential scan using standard C++ range-for syntax
    std::vector<int> scanned;
    for (const auto &item : tree) {
        scanned.push_back(item.first);
        EXPECT_EQ(item.second.GetSlotNum(), static_cast<uint32_t>(item.first));
    }

    std::vector<int> expected(total_keys);
    std::iota(expected.begin(), expected.end(), 1);
    EXPECT_EQ(scanned, expected);

    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 52: IndexIterator Begin(key) Exact Match and Lower Bound
// ============================================================================
TEST_F(BPlusTreeTest, IndexIteratorBeginWithKeyExactAndLowerBound) {
    BPlusTree<int, RID> tree("iter_begin_key", bpm_.get(), std::less<int>{}, 3, 3);

    // Insert even numbers: 10, 20, 30, 40, 50, 60, 70, 80
    for (int k = 10; k <= 80; k += 10) {
        ASSERT_TRUE(tree.Insert(k, RID(1, static_cast<uint32_t>(k))));
    }

    // Exact Match: Begin(30) should start exactly at 30
    {
        auto it = tree.Begin(30);
        ASSERT_FALSE(it.IsEnd());
        EXPECT_EQ(it->first, 30);
        std::vector<int> remaining;
        while (it != tree.End()) {
            remaining.push_back(it->first);
            ++it;
        }
        EXPECT_EQ(remaining, (std::vector<int>{30, 40, 50, 60, 70, 80}));
    }

    // Lower Bound: Begin(25) should start at 30
    {
        auto it = tree.Begin(25);
        ASSERT_FALSE(it.IsEnd());
        EXPECT_EQ(it->first, 30);
    }

    // Lower Bound before all keys: Begin(5) should start at 10
    {
        auto it = tree.Begin(5);
        ASSERT_FALSE(it.IsEnd());
        EXPECT_EQ(it->first, 10);
    }

    // Lower Bound between leaves: Begin(35) should start at 40
    {
        auto it = tree.Begin(35);
        ASSERT_FALSE(it.IsEnd());
        EXPECT_EQ(it->first, 40);
    }

    // Exact Match on last element: Begin(80)
    {
        auto it = tree.Begin(80);
        ASSERT_FALSE(it.IsEnd());
        EXPECT_EQ(it->first, 80);
        ++it;
        EXPECT_TRUE(it == tree.End());
    }

    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 53: IndexIterator Begin(key) Beyond Maximum Returns End()
// ============================================================================
TEST_F(BPlusTreeTest, IndexIteratorBeginWithKeyBeyondMaxReturnsEnd) {
    BPlusTree<int, RID> tree("iter_beyond_max", bpm_.get(), std::less<int>{}, 4, 3);

    for (int k : {10, 20, 30, 40, 50}) {
        ASSERT_TRUE(tree.Insert(k, RID(1, static_cast<uint32_t>(k))));
    }

    auto it = tree.Begin(51);
    EXPECT_TRUE(it == tree.End());

    auto it2 = tree.Begin(999);
    EXPECT_TRUE(it2 == tree.End());

    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 54: IndexIterator Copy, Move, and Postfix Increment Operators
// ============================================================================
TEST_F(BPlusTreeTest, IndexIteratorCopyMoveAndPostfixOperators) {
    BPlusTree<int, RID> tree("iter_copy_move", bpm_.get(), std::less<int>{}, 3, 3);

    for (int i = 1; i <= 20; ++i) {
        ASSERT_TRUE(tree.Insert(i, RID(1, static_cast<uint32_t>(i))));
    }

    {
        // Test copy constructor
        auto it1 = tree.Begin();
        auto it2 = it1; // Copy
        EXPECT_TRUE(it1 == it2);
        EXPECT_EQ(it1->first, it2->first);

        // Test postfix increment
        auto it3 = it1++;
        EXPECT_EQ(it3->first, 1);
        EXPECT_EQ(it1->first, 2);
        EXPECT_TRUE(it3 != it1);

        // Test move constructor
        auto it4 = std::move(it2);
        EXPECT_EQ(it4->first, 1);
        EXPECT_TRUE(it2.IsEnd());

        // Test copy assignment
        it4 = it1;
        EXPECT_EQ(it4->first, 2);

        // Test move assignment
        auto it5 = tree.Begin(10);
        it4 = std::move(it5);
        EXPECT_EQ(it4->first, 10);
        EXPECT_TRUE(it5.IsEnd());
    }

    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 55: IndexIterator Early Break Zero Pin Leaks
// ============================================================================
TEST_F(BPlusTreeTest, IndexIteratorEarlyTerminationZeroPinLeaks) {
    BPlusTree<int, RID> tree("iter_early_break", bpm_.get(), std::less<int>{}, 3, 3);

    for (int i = 1; i <= 50; ++i) {
        ASSERT_TRUE(tree.Insert(i, RID(1, static_cast<uint32_t>(i))));
    }

    // Loop and break early in the middle of a leaf page
    int seen = 0;
    for (auto it = tree.Begin(); it != tree.End(); ++it) {
        seen++;
        if (it->first == 25) {
            break; // Iterator destructed here while page is pinned!
        }
    }
    EXPECT_EQ(seen, 25);

    // Verify RAII cleanup: absolutely zero frames remain pinned
    AssertAllPagesUnpinned();

    // Loop and break at exact first entry
    for (auto it = tree.Begin(); it != tree.End(); ++it) {
        EXPECT_EQ(it->first, 1);
        break;
    }
    AssertAllPagesUnpinned();

    // Loop from Begin(key) and break early
    for (auto it = tree.Begin(30); it != tree.End(); ++it) {
        EXPECT_EQ(it->first, 30);
        break;
    }
    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 56: Range Scan - Full Scan Across Multi-Level Leaves
// ============================================================================
TEST_F(BPlusTreeTest, RangeScanFullScanAcrossMultiLevelLeaves) {
    // Fanout 4/3 forces deep tree with many leaf splits
    BPlusTree<int, RID> tree("range_full_scan", bpm_.get(), std::less<int>{}, 4, 3);

    const int total_keys = 150;
    std::vector<int> insert_keys(total_keys);
    std::iota(insert_keys.begin(), insert_keys.end(), 1);

    // Shuffle inserts to ensure leaf chain ordering is verified independent of insertion order
    std::mt19937 g(12345);
    std::shuffle(insert_keys.begin(), insert_keys.end(), g);

    for (int k : insert_keys) {
        ASSERT_TRUE(tree.Insert(k, RID(10, static_cast<uint32_t>(k))));
    }

    EXPECT_GT(tree.GetDepth(), 2u);
    EXPECT_EQ(tree.GetRecordCount(), static_cast<uint64_t>(total_keys));

    // 1. Manual iterator traversal from Begin() to End()
    std::vector<int> manual_scanned;
    for (auto it = tree.Begin(); it != tree.End(); ++it) {
        manual_scanned.push_back(it->first);
        EXPECT_EQ(it->second.GetSlotNum(), static_cast<uint32_t>(it->first));
    }

    std::vector<int> expected_sorted(total_keys);
    std::iota(expected_sorted.begin(), expected_sorted.end(), 1);
    EXPECT_EQ(manual_scanned, expected_sorted);

    // 2. Range-based for loop syntax
    std::vector<int> range_for_scanned;
    for (const auto &[k, val] : tree) {
        range_for_scanned.push_back(k);
        EXPECT_EQ(val.GetSlotNum(), static_cast<uint32_t>(k));
    }
    EXPECT_EQ(range_for_scanned, expected_sorted);

    // 3. Tree RangeScan helper
    std::vector<std::pair<int, RID>> helper_scanned;
    size_t count = tree.RangeScan(1, total_keys, &helper_scanned);
    EXPECT_EQ(count, static_cast<size_t>(total_keys));
    EXPECT_EQ(helper_scanned.size(), static_cast<size_t>(total_keys));
    for (int i = 0; i < total_keys; ++i) {
        EXPECT_EQ(helper_scanned[i].first, expected_sorted[i]);
    }

    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 57: Range Scan - From a Key (Exact Hits, Lower Bounds, Subranges)
// ============================================================================
TEST_F(BPlusTreeTest, RangeScanFromKeyExactAndSubranges) {
    BPlusTree<int, RID> tree("range_from_key", bpm_.get(), std::less<int>{}, 4, 3);

    // Insert keys: 10, 20, 30, ..., 200 (20 keys spaced by 10)
    for (int k = 10; k <= 200; k += 10) {
        ASSERT_TRUE(tree.Insert(k, RID(1, static_cast<uint32_t>(k))));
    }

    // 1. Exact Hit: start scan from 70 to end
    {
        std::vector<int> scanned;
        for (auto it = tree.Begin(70); it != tree.End(); ++it) {
            scanned.push_back(it->first);
        }
        std::vector<int> expected;
        for (int k = 70; k <= 200; k += 10) expected.push_back(k);
        EXPECT_EQ(scanned, expected);
    }

    // 2. Lower Bound Hit on missing key: start scan from 45 (should land on 50)
    {
        std::vector<int> scanned;
        for (auto it = tree.Begin(45); it != tree.End(); ++it) {
            scanned.push_back(it->first);
        }
        std::vector<int> expected;
        for (int k = 50; k <= 200; k += 10) expected.push_back(k);
        EXPECT_EQ(scanned, expected);
    }

    // 3. Bounded Subrange [35, 125]: should match 40, 50, ..., 120
    {
        std::vector<int> scanned;
        for (auto it = tree.Begin(35); it != tree.End() && it->first <= 125; ++it) {
            scanned.push_back(it->first);
        }
        std::vector<int> expected = {40, 50, 60, 70, 80, 90, 100, 110, 120};
        EXPECT_EQ(scanned, expected);
    }

    // 4. Start before minimum key: Begin(-50) should start at first key (10)
    {
        auto it = tree.Begin(-50);
        ASSERT_FALSE(it.IsEnd());
        EXPECT_EQ(it->first, 10);
    }

    // 5. Start at minimum key: Begin(10) should start at 10
    {
        auto it = tree.Begin(10);
        ASSERT_FALSE(it.IsEnd());
        EXPECT_EQ(it->first, 10);
    }

    // 6. Single-key range using RangeScan helper [90, 90]
    {
        std::vector<std::pair<int, RID>> result;
        size_t count = tree.RangeScan(90, 90, &result);
        EXPECT_EQ(count, 1u);
        ASSERT_EQ(result.size(), 1u);
        EXPECT_EQ(result[0].first, 90);
    }

    // 7. Missing single-key range [85, 85]
    {
        std::vector<std::pair<int, RID>> result;
        size_t count = tree.RangeScan(85, 85, &result);
        EXPECT_EQ(count, 0u);
        EXPECT_TRUE(result.empty());
    }

    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 58: Range Scan - Past the End
// ============================================================================
TEST_F(BPlusTreeTest, RangeScanPastTheEnd) {
    BPlusTree<int, RID> tree("range_past_end", bpm_.get(), std::less<int>{}, 4, 3);

    for (int k : {10, 20, 30, 40, 50}) {
        ASSERT_TRUE(tree.Insert(k, RID(1, static_cast<uint32_t>(k))));
    }

    // 1. Begin(51): one past the maximum key (50)
    {
        auto it = tree.Begin(51);
        EXPECT_TRUE(it == tree.End());
        EXPECT_FALSE(it != tree.End());
        EXPECT_TRUE(it.IsEnd());

        int count = 0;
        for (; it != tree.End(); ++it) {
            count++;
        }
        EXPECT_EQ(count, 0);
    }

    // 2. Begin with extreme key
    {
        auto it = tree.Begin(std::numeric_limits<int>::max());
        EXPECT_TRUE(it == tree.End());
    }

    // 3. RangeScan helper past the end [51, 100]
    {
        std::vector<std::pair<int, RID>> result;
        size_t count = tree.RangeScan(51, 100, &result);
        EXPECT_EQ(count, 0u);
        EXPECT_TRUE(result.empty());
    }

    // 4. Inverted range [100, 50] (low_key > high_key)
    {
        std::vector<std::pair<int, RID>> result;
        size_t count = tree.RangeScan(100, 50, &result);
        EXPECT_EQ(count, 0u);
        EXPECT_TRUE(result.empty());
    }

    // 5. Scan starting at exact maximum key (50): iterates once and reaches End()
    {
        auto it = tree.Begin(50);
        ASSERT_FALSE(it.IsEnd());
        EXPECT_EQ(it->first, 50);
        ++it;
        EXPECT_TRUE(it == tree.End());
    }

    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 59: Range Scan - Empty Tree (Fresh and Emptied Lifecycle)
// ============================================================================
TEST_F(BPlusTreeTest, RangeScanOnEmptyTree) {
    // Part 1: Fresh Empty Tree
    {
        BPlusTree<int, RID> empty_tree("fresh_empty", bpm_.get(), std::less<int>{}, 4, 3);

        EXPECT_TRUE(empty_tree.IsEmpty());
        EXPECT_TRUE(empty_tree.Begin() == empty_tree.End());
        EXPECT_TRUE(empty_tree.Begin(0) == empty_tree.End());
        EXPECT_TRUE(empty_tree.Begin(-100) == empty_tree.End());
        EXPECT_TRUE(empty_tree.Begin(100) == empty_tree.End());

        int count = 0;
        for (auto it = empty_tree.Begin(); it != empty_tree.End(); ++it) {
            count++;
        }
        EXPECT_EQ(count, 0);

        for (auto it = empty_tree.Begin(50); it != empty_tree.End(); ++it) {
            count++;
        }
        EXPECT_EQ(count, 0);

        std::vector<std::pair<int, RID>> result;
        EXPECT_EQ(empty_tree.RangeScan(1, 100, &result), 0u);
        EXPECT_TRUE(result.empty());

        AssertAllPagesUnpinned();
    }

    // Part 2: Emptied Tree Lifecycle (Inserted -> Deleted Down to Empty)
    {
        BPlusTree<int, RID> dynamic_tree("emptied_lifecycle", bpm_.get(), std::less<int>{}, 3, 3);

        std::vector<int> keys = {5, 10, 15, 20, 25};
        for (int k : keys) {
            ASSERT_TRUE(dynamic_tree.Insert(k, RID(1, static_cast<uint32_t>(k))));
        }
        EXPECT_FALSE(dynamic_tree.IsEmpty());
        EXPECT_EQ(dynamic_tree.GetRecordCount(), 5u);

        // Delete all keys to return tree to empty state
        for (int k : keys) {
            ASSERT_TRUE(dynamic_tree.Remove(k));
        }

        EXPECT_TRUE(dynamic_tree.IsEmpty());
        EXPECT_EQ(dynamic_tree.GetRecordCount(), 0u);
        EXPECT_EQ(dynamic_tree.GetDepth(), 0u);
        EXPECT_EQ(dynamic_tree.GetRootPageId(), INVALID_PAGE_ID);

        // Range scan operations on the emptied tree
        EXPECT_TRUE(dynamic_tree.Begin() == dynamic_tree.End());
        EXPECT_TRUE(dynamic_tree.Begin(10) == dynamic_tree.End());
        EXPECT_TRUE(dynamic_tree.Begin(100) == dynamic_tree.End());

        int count = 0;
        for (const auto &item : dynamic_tree) {
            (void)item;
            count++;
        }
        EXPECT_EQ(count, 0);

        std::vector<std::pair<int, RID>> result;
        EXPECT_EQ(dynamic_tree.RangeScan(0, 100, &result), 0u);
        EXPECT_TRUE(result.empty());

        AssertAllPagesUnpinned();
    }
}

// ============================================================================
// Test 60: Benchmark & Buffer Pool I/O Profiling (Bulk Insert, Lookup, Range Scan)
// ============================================================================
TEST_F(BPlusTreeTest, BenchmarkBulkInsertPointLookupRangeScanWithBufferPoolIO) {
    const size_t num_keys = 5000;
    std::string bench_db = "test_b_plus_tree_bench_io.db";
    if (fs::exists(bench_db)) {
        fs::remove(bench_db);
    }

    auto disk = std::make_unique<DiskManager>(bench_db);
    auto bpm = std::make_unique<BufferPoolManager>(1000, disk.get());

    {
        BPlusTree<int, RID> tree("bench_tree", bpm.get(), std::less<int>{}, 32, 32);

        // --- 1. Bulk Insert ---
        bpm->ResetStats();
        auto start_insert = std::chrono::high_resolution_clock::now();
        for (size_t i = 1; i <= num_keys; ++i) {
            int key = static_cast<int>(i);
            ASSERT_TRUE(tree.Insert(key, RID(static_cast<page_id_t>(key / 100 + 1), static_cast<uint32_t>(key))));
        }
        auto end_insert = std::chrono::high_resolution_clock::now();
        double insert_ms = std::chrono::duration<double, std::milli>(end_insert - start_insert).count();

        BufferPoolStats insert_stats = bpm->GetStats();
        EXPECT_EQ(tree.GetRecordCount(), num_keys);
        EXPECT_GT(insert_stats.fetch_count, 0u);
        EXPECT_GT(insert_stats.hit_count, 0u);
        EXPECT_GT(insert_stats.new_page_count, 0u);
        EXPECT_GT(insert_ms, 0.0);

        // --- 2. Point Lookups (Hits) ---
        bpm->ResetStats();
        auto start_lookup = std::chrono::high_resolution_clock::now();
        size_t lookup_hits = 0;
        RID rid;
        for (size_t i = 1; i <= num_keys; ++i) {
            if (tree.GetValue(static_cast<int>(i), &rid)) {
                lookup_hits++;
            }
        }
        auto end_lookup = std::chrono::high_resolution_clock::now();
        double lookup_ms = std::chrono::duration<double, std::milli>(end_lookup - start_lookup).count();

        BufferPoolStats lookup_stats = bpm->GetStats();
        EXPECT_EQ(lookup_hits, num_keys);
        EXPECT_EQ(lookup_stats.hit_count, lookup_stats.fetch_count); // 100% cache hit ratio in memory
        EXPECT_DOUBLE_EQ(lookup_stats.HitRatio(), 100.0);
        EXPECT_EQ(lookup_stats.miss_count, 0u);
        EXPECT_GT(lookup_ms, 0.0);

        // --- 3. Point Lookups (Misses / Non-existent keys) ---
        bpm->ResetStats();
        size_t lookup_misses = 0;
        for (size_t i = 1; i <= 1000; ++i) {
            int non_existent_key = static_cast<int>(num_keys + i);
            if (!tree.GetValue(non_existent_key, &rid)) {
                lookup_misses++;
            }
        }
        BufferPoolStats miss_stats = bpm->GetStats();
        EXPECT_EQ(lookup_misses, 1000u);
        EXPECT_GT(miss_stats.fetch_count, 0u);
        EXPECT_DOUBLE_EQ(miss_stats.HitRatio(), 100.0);

        // --- 4. Range Scans (Subranges & Full Scan) ---
        bpm->ResetStats();
        auto start_scan = std::chrono::high_resolution_clock::now();
        size_t total_scanned = 0;
        std::vector<std::pair<int, RID>> scan_results;
        for (int start = 1; start <= 4000; start += 500) {
            scan_results.clear();
            total_scanned += tree.RangeScan(start, start + 99, &scan_results);
            EXPECT_EQ(scan_results.size(), 100u);
        }
        auto end_scan = std::chrono::high_resolution_clock::now();
        double scan_ms = std::chrono::duration<double, std::milli>(end_scan - start_scan).count();

        BufferPoolStats scan_stats = bpm->GetStats();
        EXPECT_EQ(total_scanned, 800u);
        EXPECT_GT(scan_stats.fetch_count, 0u);
        EXPECT_GT(scan_stats.hit_count, 0u);
        EXPECT_GT(scan_ms, 0.0);

        // Full leaf chain scan verification
        size_t full_scan_items = 0;
        for (auto it = tree.Begin(); it != tree.End(); ++it) {
            full_scan_items++;
        }
        EXPECT_EQ(full_scan_items, num_keys);
    }

    // Verify all buffer frames cleanly unpinned
    for (size_t f = 0; f < bpm->GetPoolSize(); ++f) {
        Page *page = bpm->GetPage(static_cast<frame_id_t>(f));
        if (page != nullptr) {
            EXPECT_EQ(page->GetPinCount(), 0);
        }
    }

    bpm.reset();
    disk.reset();
    if (fs::exists(bench_db)) {
        fs::remove(bench_db);
    }
}

// ============================================================================
// Test 61: Persistence Test - Build, Flush, Close, Reopen, Verify Data & Root ID
// ============================================================================
TEST_F(BPlusTreeTest, PersistenceBuildFlushCloseReopenVerifyDataAndRootId) {
    const std::string persist_db = "persist_verify_root_and_data.db";
    if (fs::exists(persist_db)) {
        fs::remove(persist_db);
    }

    page_id_t header_pid = INVALID_PAGE_ID;
    page_id_t expected_root_pid = INVALID_PAGE_ID;
    const std::vector<int> initial_keys = {
        10, 20, 30, 40, 50, 60, 70, 80, 90, 100,
        110, 120, 130, 140, 150, 160, 170, 180, 190, 200,
        210, 220, 230, 240, 250, 260, 270, 280, 290, 300,
        310, 320, 330, 340, 350, 360, 370, 380, 390, 400
    };

    // ------------------------------------------------------------------------
    // Phase 1: Build multi-level tree, capture root ID, flush, and close
    // ------------------------------------------------------------------------
    {
        auto disk = std::make_unique<DiskManager>(persist_db);
        auto bpm = std::make_unique<BufferPoolManager>(50, disk.get());

        // Small fanout (leaf_max = 4, internal_max = 3) to force multi-level tree with internal splits
        BPlusTree<int, RID> tree("persist_tree", bpm.get(), std::less<int>{}, 4, 3);
        header_pid = tree.GetHeaderPageId();
        ASSERT_NE(header_pid, INVALID_PAGE_ID);

        // Initially empty tree has INVALID_PAGE_ID as root
        EXPECT_EQ(tree.GetRootPageId(), INVALID_PAGE_ID);

        for (int k : initial_keys) {
            RID rid(static_cast<page_id_t>(k / 10), static_cast<uint32_t>(k));
            ASSERT_TRUE(tree.Insert(k, rid));
        }

        EXPECT_EQ(tree.GetRecordCount(), initial_keys.size());
        EXPECT_TRUE(tree.ValidateTree().is_valid);

        // Capture persisted root page ID
        expected_root_pid = tree.GetRootPageId();
        EXPECT_NE(expected_root_pid, INVALID_PAGE_ID);
        EXPECT_NE(expected_root_pid, header_pid);

        // Flush all dirty pages to physical storage
        bpm->FlushAllPages();

        // Verify no pinned pages before teardown
        for (size_t f = 0; f < bpm->GetPoolSize(); ++f) {
            Page *page = bpm->GetPage(static_cast<frame_id_t>(f));
            if (page != nullptr) {
                EXPECT_EQ(page->GetPinCount(), 0);
            }
        }
    } // DiskManager and BufferPoolManager fully closed and destroyed

    // ------------------------------------------------------------------------
    // Phase 2: Reopen from disk, verify root ID, metadata, point lookups & scan
    // ------------------------------------------------------------------------
    {
        auto disk = std::make_unique<DiskManager>(persist_db);
        auto bpm = std::make_unique<BufferPoolManager>(50, disk.get());

        // Attach to existing tree using header_pid
        BPlusTree<int, RID> tree("persist_tree", header_pid, bpm.get(), std::less<int>{}, 4, 3);

        // 1. Verify root ID matches exactly what was built before closing
        EXPECT_EQ(tree.GetRootPageId(), expected_root_pid);
        EXPECT_EQ(tree.GetHeaderPageId(), header_pid);
        EXPECT_FALSE(tree.IsEmpty());
        EXPECT_EQ(tree.GetRecordCount(), initial_keys.size());

        // 2. Verify tree structural invariants post-reopen
        auto val = tree.ValidateTree();
        EXPECT_TRUE(val.is_valid) << "Reopened tree validation failed: " << val.error_message;

        // 3. Verify point lookups for all persisted keys
        for (int k : initial_keys) {
            RID found_rid;
            ASSERT_TRUE(tree.GetValue(k, &found_rid)) << "Key " << k << " missing after reopen!";
            EXPECT_EQ(found_rid.GetPageId(), static_cast<page_id_t>(k / 10));
            EXPECT_EQ(found_rid.GetSlotNum(), static_cast<uint32_t>(k));
        }

        // Verify non-existent keys return false
        RID missing_rid;
        EXPECT_FALSE(tree.GetValue(5, &missing_rid));
        EXPECT_FALSE(tree.GetValue(999, &missing_rid));

        // 4. Verify full leaf chain traversal via IndexIterator
        std::vector<int> scanned_keys;
        for (auto it = tree.Begin(); it != tree.End(); ++it) {
            scanned_keys.push_back(it->first);
            EXPECT_EQ(it->second.GetSlotNum(), static_cast<uint32_t>(it->first));
        }
        EXPECT_EQ(scanned_keys, initial_keys);

        // 5. Verify range scans on reopened tree
        std::vector<std::pair<int, RID>> range_res;
        tree.RangeScan(100, 200, &range_res);
        EXPECT_EQ(range_res.size(), 11u);
        EXPECT_EQ(range_res.front().first, 100);
        EXPECT_EQ(range_res.back().first, 200);

        // 6. Mutate the reopened tree: insert additional keys and delete some keys
        ASSERT_TRUE(tree.Insert(410, RID(41, 410)));
        ASSERT_TRUE(tree.Insert(420, RID(42, 420)));
        ASSERT_TRUE(tree.Insert(430, RID(43, 430)));
        ASSERT_TRUE(tree.Remove(10));
        ASSERT_TRUE(tree.Remove(20));

        EXPECT_EQ(tree.GetRecordCount(), initial_keys.size() + 3 - 2);
        EXPECT_TRUE(tree.ValidateTree().is_valid);

        // Capture updated root ID (may remain same or change if split/collapse occurred)
        expected_root_pid = tree.GetRootPageId();
        EXPECT_NE(expected_root_pid, INVALID_PAGE_ID);

        // Flush modified pages to disk
        bpm->FlushAllPages();

        for (size_t f = 0; f < bpm->GetPoolSize(); ++f) {
            Page *page = bpm->GetPage(static_cast<frame_id_t>(f));
            if (page != nullptr) {
                EXPECT_EQ(page->GetPinCount(), 0);
            }
        }
    } // Closed second time

    // ------------------------------------------------------------------------
    // Phase 3: Second reopen from disk, verify updated root ID and mutated data
    // ------------------------------------------------------------------------
    {
        auto disk = std::make_unique<DiskManager>(persist_db);
        auto bpm = std::make_unique<BufferPoolManager>(50, disk.get());

        BPlusTree<int, RID> tree("persist_tree", header_pid, bpm.get(), std::less<int>{}, 4, 3);

        // Verify root ID matches updated root
        EXPECT_EQ(tree.GetRootPageId(), expected_root_pid);
        EXPECT_EQ(tree.GetRecordCount(), initial_keys.size() + 1);

        // Deleted keys must not exist
        RID r;
        EXPECT_FALSE(tree.GetValue(10, &r));
        EXPECT_FALSE(tree.GetValue(20, &r));

        // Newly added keys must exist
        EXPECT_TRUE(tree.GetValue(410, &r));
        EXPECT_TRUE(tree.GetValue(420, &r));
        EXPECT_TRUE(tree.GetValue(430, &r));

        EXPECT_TRUE(tree.ValidateTree().is_valid);

        for (size_t f = 0; f < bpm->GetPoolSize(); ++f) {
            Page *page = bpm->GetPage(static_cast<frame_id_t>(f));
            if (page != nullptr) {
                EXPECT_EQ(page->GetPinCount(), 0);
            }
        }
    } // Closed third time

    // ------------------------------------------------------------------------
    // Phase 4: Empty Tree Persistence (Build empty, flush, close, reopen, verify)
    // ------------------------------------------------------------------------
    {
        std::string empty_persist_db = "persist_empty_tree.db";
        if (fs::exists(empty_persist_db)) {
            fs::remove(empty_persist_db);
        }

        page_id_t empty_header_pid = INVALID_PAGE_ID;

        // Build empty tree
        {
            auto disk = std::make_unique<DiskManager>(empty_persist_db);
            auto bpm = std::make_unique<BufferPoolManager>(10, disk.get());

            BPlusTree<int, RID> empty_tree("empty_tree", bpm.get(), std::less<int>{}, 4, 3);
            empty_header_pid = empty_tree.GetHeaderPageId();
            ASSERT_NE(empty_header_pid, INVALID_PAGE_ID);
            EXPECT_EQ(empty_tree.GetRootPageId(), INVALID_PAGE_ID);
            EXPECT_TRUE(empty_tree.IsEmpty());

            bpm->FlushAllPages();
        }

        // Reopen empty tree
        {
            auto disk = std::make_unique<DiskManager>(empty_persist_db);
            auto bpm = std::make_unique<BufferPoolManager>(10, disk.get());

            BPlusTree<int, RID> empty_tree("empty_tree", empty_header_pid, bpm.get(), std::less<int>{}, 4, 3);
            EXPECT_EQ(empty_tree.GetRootPageId(), INVALID_PAGE_ID);
            EXPECT_TRUE(empty_tree.IsEmpty());
            EXPECT_EQ(empty_tree.GetRecordCount(), 0u);
            EXPECT_TRUE(empty_tree.Begin() == empty_tree.End());

            // Insert first key into reopened empty tree
            ASSERT_TRUE(empty_tree.Insert(99, RID(9, 99)));
            EXPECT_NE(empty_tree.GetRootPageId(), INVALID_PAGE_ID);
            EXPECT_FALSE(empty_tree.IsEmpty());
            EXPECT_EQ(empty_tree.GetRecordCount(), 1u);

            page_id_t new_root_pid = empty_tree.GetRootPageId();
            bpm->FlushAllPages();

            // Reopen once more to verify new root persisted
            BPlusTree<int, RID> reopened_single("empty_tree", empty_header_pid, bpm.get(), std::less<int>{}, 4, 3);
            EXPECT_EQ(reopened_single.GetRootPageId(), new_root_pid);
            EXPECT_EQ(reopened_single.GetRecordCount(), 1u);
            RID r;
            EXPECT_TRUE(reopened_single.GetValue(99, &r));
            EXPECT_EQ(r.GetSlotNum(), 99u);
        }

        if (fs::exists(empty_persist_db)) {
            fs::remove(empty_persist_db);
        }
    }

    // Cleanup main test db
    if (fs::exists(persist_db)) {
        fs::remove(persist_db);
    }
}
