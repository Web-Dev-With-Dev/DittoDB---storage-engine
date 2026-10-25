#include <gtest/gtest.h>
#include <filesystem>
#include <memory>
#include <vector>
#include <string>

#include "b_plus_tree_header_page.h"
#include "b_plus_tree_internal_page.h"
#include "b_plus_tree_leaf_page.h"
#include "buffer_pool_manager.h"
#include "disk_manager.h"
#include "page.h"
#include "rid.h"

class BPlusTreeHeaderPageTest : public ::testing::Test {
protected:
    const std::string db_file_ = "test_header_persistence.db";

    void SetUp() override {
        if (std::filesystem::exists(db_file_)) {
            std::filesystem::remove(db_file_);
        }
    }

    void TearDown() override {
        if (std::filesystem::exists(db_file_)) {
            std::filesystem::remove(db_file_);
        }
    }
};

// ============================================================================
// Test 1: Sizing and Physical Standard Layout
// ============================================================================
TEST_F(BPlusTreeHeaderPageTest, LayoutAndSizing) {
    EXPECT_EQ(sizeof(BPlusTreeHeaderPage), PAGE_SIZE);
    EXPECT_TRUE(std::is_standard_layout_v<BPlusTreeHeaderPage>);

    EXPECT_EQ(BPlusTreeHeaderPage::OFFSET_PAGE_TYPE, 0);
    EXPECT_EQ(BPlusTreeHeaderPage::OFFSET_LSN, 4);
    EXPECT_EQ(BPlusTreeHeaderPage::OFFSET_ROOT_PAGE_ID, 8);
    EXPECT_EQ(BPlusTreeHeaderPage::OFFSET_HEADER_PAGE_ID, 12);
    EXPECT_EQ(BPlusTreeHeaderPage::OFFSET_KEY_SIZE, 16);
    EXPECT_EQ(BPlusTreeHeaderPage::OFFSET_DEPTH, 20);
    EXPECT_EQ(BPlusTreeHeaderPage::OFFSET_RECORD_COUNT, 24);
}

// ============================================================================
// Test 2: Initialization and Default State
// ============================================================================
TEST_F(BPlusTreeHeaderPageTest, Initialization) {
    Page page;
    BPlusTreeHeaderPage *header = BPlusTreeHeaderPage::Cast(&page);
    ASSERT_NE(header, nullptr);

    header->Init(0, INVALID_PAGE_ID, 8);

    EXPECT_EQ(header->GetPageType(), PageType::BPLUS_TREE_HEADER_PAGE);
    EXPECT_EQ(header->GetHeaderPageId(), 0);
    EXPECT_EQ(header->GetRootPageId(), INVALID_PAGE_ID);
    EXPECT_FALSE(header->HasRoot());
    EXPECT_EQ(header->GetKeySize(), 8);
    EXPECT_EQ(header->GetDepth(), 0);
    EXPECT_EQ(header->GetRecordCount(), 0);
    EXPECT_EQ(header->GetLSN(), 0);
}

// ============================================================================
// Test 3: Root Page ID Transitions (Lifecycle from Leaf to Internal Root)
// ============================================================================
TEST_F(BPlusTreeHeaderPageTest, RootPageTransitions) {
    Page page;
    BPlusTreeHeaderPage *header = BPlusTreeHeaderPage::Cast(&page);
    header->Init(0);

    // Initial state: empty tree
    EXPECT_FALSE(header->HasRoot());
    EXPECT_EQ(header->GetRootPageId(), INVALID_PAGE_ID);
    EXPECT_EQ(header->GetDepth(), 0);

    // 1. First insert creates root leaf page (e.g. page_id = 1)
    header->SetRootPageId(1);
    EXPECT_TRUE(header->HasRoot());
    EXPECT_EQ(header->GetRootPageId(), 1);
    EXPECT_EQ(header->GetDepth(), 1);

    // 2. Leaf splits, creating new root internal page (page_id = 3)
    header->SetRootPageId(3);
    header->SetDepth(2);
    EXPECT_EQ(header->GetRootPageId(), 3);
    EXPECT_EQ(header->GetDepth(), 2);

    // 3. Tree deepens to depth 3 (page_id = 7)
    header->SetRootPageId(7);
    header->SetDepth(3);
    EXPECT_EQ(header->GetRootPageId(), 7);
    EXPECT_EQ(header->GetDepth(), 3);

    // 4. Record count increments
    header->IncreaseRecordCount(500);
    EXPECT_EQ(header->GetRecordCount(), 500);
    header->IncreaseRecordCount(-100);
    EXPECT_EQ(header->GetRecordCount(), 400);

    // 5. Tree emptied
    header->SetRootPageId(INVALID_PAGE_ID);
    EXPECT_FALSE(header->HasRoot());
    EXPECT_EQ(header->GetRootPageId(), INVALID_PAGE_ID);
    EXPECT_EQ(header->GetDepth(), 0);
}

// ============================================================================
// Test 4: Full Disk Persistence and Crash-Restart Recovery
// ============================================================================
TEST_F(BPlusTreeHeaderPageTest, DiskPersistenceAcrossRestart) {
    page_id_t header_pid = INVALID_PAGE_ID;

    // Phase 1: Initialize header page in BufferPoolManager and persist to disk
    {
        auto disk_manager = std::make_unique<DiskManager>(db_file_);
        auto bpm = std::make_unique<BufferPoolManager>(10, disk_manager.get());

        Page *raw_header = bpm->NewPage(&header_pid);
        ASSERT_NE(raw_header, nullptr);
        EXPECT_EQ(header_pid, 0);

        BPlusTreeHeaderPage *header = BPlusTreeHeaderPage::Cast(raw_header);
        header->Init(header_pid, 1, 8);
        header->SetRootPageId(42);
        header->SetDepth(4);
        header->SetRecordCount(15000);
        header->SetLSN(9999);

        // Unpin and flush
        bpm->UnpinPage(header_pid, true);
        bpm->FlushPage(header_pid);
    } // BufferPoolManager and DiskManager destroyed

    // Phase 2: Restart engine from disk file and verify root_page_id restored
    {
        auto disk_manager = std::make_unique<DiskManager>(db_file_);
        auto bpm = std::make_unique<BufferPoolManager>(10, disk_manager.get());

        Page *raw_header = bpm->FetchPage(header_pid);
        ASSERT_NE(raw_header, nullptr);

        const BPlusTreeHeaderPage *header = BPlusTreeHeaderPage::Cast(raw_header);
        EXPECT_EQ(header->GetPageType(), PageType::BPLUS_TREE_HEADER_PAGE);
        EXPECT_EQ(header->GetHeaderPageId(), header_pid);
        EXPECT_EQ(header->GetRootPageId(), 42);
        EXPECT_TRUE(header->HasRoot());
        EXPECT_EQ(header->GetDepth(), 4);
        EXPECT_EQ(header->GetRecordCount(), 15000);
        EXPECT_EQ(header->GetLSN(), 9999);

        bpm->UnpinPage(header_pid, false);
    }
}

// ============================================================================
// Test 5: Isolated Leaf and Internal Node Testing with Header Page
// ============================================================================
TEST_F(BPlusTreeHeaderPageTest, IsolatedNodesWithHeaderPage) {
    using LeafPage = BPlusTreeLeafPage<int, RID>;
    using InternalPage = BPlusTreeInternalPage<int, page_id_t>;

    Page header_frame;
    Page leaf1_frame;
    Page leaf2_frame;
    Page root_internal_frame;

    // 1. Initialize Header Page
    BPlusTreeHeaderPage *header = BPlusTreeHeaderPage::Cast(&header_frame);
    header->Init(0);
    EXPECT_FALSE(header->HasRoot());

    // 2. Unit test Leaf Node 1 in isolation
    LeafPage *leaf1 = LeafPage::Cast(&leaf1_frame);
    leaf1->Init(1, INVALID_PAGE_ID, 4);

    // Associate initial root with leaf1
    header->SetRootPageId(leaf1->GetPageId());
    EXPECT_EQ(header->GetRootPageId(), 1);
    EXPECT_EQ(header->GetDepth(), 1);

    // Insert records up to capacity
    leaf1->Insert(10, RID(1, 1));
    leaf1->Insert(20, RID(1, 2));
    leaf1->Insert(30, RID(1, 3));
    leaf1->Insert(40, RID(1, 4));
    EXPECT_EQ(leaf1->GetSize(), 4);
    header->SetRecordCount(4);

    // 3. Leaf Node split in isolation
    LeafPage *leaf2 = LeafPage::Cast(&leaf2_frame);
    leaf2->Init(2, INVALID_PAGE_ID, 4);

    leaf1->MoveHalfTo(leaf2);
    EXPECT_EQ(leaf1->GetSize(), 2);
    EXPECT_EQ(leaf2->GetSize(), 2);
    EXPECT_EQ(leaf1->GetNextPageId(), 2);
    EXPECT_EQ(leaf2->GetNextPageId(), INVALID_PAGE_ID);

    // Keys in leaf1: 10, 20. Keys in leaf2: 30, 40.
    int split_key = leaf2->KeyAt(0);
    EXPECT_EQ(split_key, 30);

    // 4. Unit test Internal Node in isolation
    InternalPage *root_internal = InternalPage::Cast(&root_internal_frame);
    root_internal->Init(3, INVALID_PAGE_ID, 10);
    root_internal->Populate(leaf1->GetPageId(), split_key, leaf2->GetPageId());

    // Update parent pointers on leaves
    leaf1->SetParentPageId(root_internal->GetPageId());
    leaf2->SetParentPageId(root_internal->GetPageId());

    // Update Header page to point to new Internal Root
    header->SetRootPageId(root_internal->GetPageId());
    header->SetDepth(2);
    EXPECT_EQ(header->GetRootPageId(), 3);
    EXPECT_EQ(header->GetDepth(), 2);

    // Verify binary-search routing in internal node
    EXPECT_EQ(root_internal->Lookup(5), 1);   // < 30 -> leaf 1
    EXPECT_EQ(root_internal->Lookup(20), 1);  // < 30 -> leaf 1
    EXPECT_EQ(root_internal->Lookup(30), 2);  // >= 30 -> leaf 2
    EXPECT_EQ(root_internal->Lookup(100), 2); // >= 30 -> leaf 2

    // Verify lookup on resolved leaves
    RID found_rid;
    page_id_t target_leaf_pid = root_internal->Lookup(20);
    EXPECT_EQ(target_leaf_pid, 1);
    EXPECT_TRUE(leaf1->Lookup(20, &found_rid));
    EXPECT_EQ(found_rid, RID(1, 2));

    target_leaf_pid = root_internal->Lookup(40);
    EXPECT_EQ(target_leaf_pid, 2);
    EXPECT_TRUE(leaf2->Lookup(40, &found_rid));
    EXPECT_EQ(found_rid, RID(1, 4));
}
