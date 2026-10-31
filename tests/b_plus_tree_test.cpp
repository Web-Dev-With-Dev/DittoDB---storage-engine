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
// Test 6: Manually Constructed Multi-Level Tree (Traverse Root to Leaf)
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
// Test 7: Dynamic Insert, Splits, and Large Scale GetValue Lookups
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
// Test 8: Vector Overload Compatibility (CMU 15-445 Parity)
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
// Test 9: GenericKey and GenericComparator Integration
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

    AssertAllPagesUnpinned();
}

// ============================================================================
// Test 10: Persistence and Crash Restart
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

        for (size_t i = 0; i < bpm->GetPoolSize(); ++i) {
            Page *page = bpm->GetPage(static_cast<frame_id_t>(i));
            if (page != nullptr) {
                EXPECT_EQ(page->GetPinCount(), 0);
            }
        }
    }
}
