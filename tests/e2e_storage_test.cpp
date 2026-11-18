#include <gtest/gtest.h>
#include <algorithm>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "buffer_pool_manager.h"
#include "config.h"
#include "disk_manager.h"
#include "rid.h"
#include "table_heap.h"
#include "table_page.h"
#include "tuple.h"

namespace fs = std::filesystem;


/**
 * @brief End-to-End Multi-Tier Storage Integration Tests.
 *
 * Verifies the cooperative behavior of the entire storage stack:
 *   DiskManager (Tier 1)
 *   LRUKReplacer (Tier 2A)
 *   Page Frames & Latches (Tier 2B)
 *   BufferPoolManager (Tier 3)
 *   TablePage & Slotted Pages (Tier 4)
 *   TableHeap & Cross-Page Migration (Tier 5)
 *
 * Forces heavy buffer pool evictions by operating on datasets spanning
 * many more pages (30-50 pages) than available buffer frames (4-6 frames).
 */
class EndToEndStorageIntegrationTest : public ::testing::Test {
protected:
    const std::string test_db_ = "test_e2e_storage.db";

    void SetUp() override {
        if (fs::exists(test_db_)) {
            fs::remove(test_db_);
        }
    }

    void TearDown() override {
        if (fs::exists(test_db_)) {
            fs::remove(test_db_);
        }
    }

    static Tuple CreatePayloadTuple(const std::string &str) {
        return Tuple(std::vector<char>(str.begin(), str.end()));
    }

    static std::string TupleToString(const Tuple &tuple) {
        return std::string(tuple.GetData(), tuple.GetData() + tuple.GetLength());
    }
};

// ============================================================================
// Test 1: Direct TablePage Multi-Page Stress with Forced Buffer Evictions
// ============================================================================
TEST_F(EndToEndStorageIntegrationTest, ForcedEvictionDirectTablePages) {
    constexpr size_t BUFFER_POOL_SIZE = 5;
    constexpr size_t NUM_PAGES = 30; // 6x buffer pool size
    constexpr size_t TUPLES_PER_PAGE = 8;

    std::map<RID, std::string> inserted_records;
    std::vector<page_id_t> page_ids;

    auto disk_manager = std::make_unique<DiskManager>(test_db_);
    auto bpm = std::make_unique<BufferPoolManager>(BUFFER_POOL_SIZE, disk_manager.get());

    // 1. Allocate 30 pages through BPM (forcing at least 25 evictions)
    for (size_t p = 0; p < NUM_PAGES; ++p) {
        page_id_t page_id = INVALID_PAGE_ID;
        Page *raw_page = bpm->NewPage(&page_id);
        ASSERT_NE(raw_page, nullptr) << "Failed to allocate page " << p;
        page_ids.push_back(page_id);

        auto *table_page = reinterpret_cast<TablePage *>(raw_page);
        table_page->Init(page_id);

        // Insert tuples on this page
        for (size_t t = 0; t < TUPLES_PER_PAGE; ++t) {
            std::string content = "Page_" + std::to_string(page_id) + "_Tuple_" + std::to_string(t) +
                                  "_Payload_XYZ_" + std::string(60, static_cast<char>('A' + (t % 26)));
            Tuple tuple = CreatePayloadTuple(content);
            RID rid;
            ASSERT_TRUE(table_page->InsertTuple(tuple, &rid))
                << "Insert failed on page " << page_id << ", tuple " << t;
            inserted_records[rid] = content;
        }

        // Unpin page as dirty so it can be evicted by LRU-K
        bpm->UnpinPage(page_id, true);
    }

    EXPECT_EQ(inserted_records.size(), NUM_PAGES * TUPLES_PER_PAGE);

    // 2. Read back all tuples in completely shuffled/random order.
    // Every read accesses pages that were evicted to disk, forcing BPM to
    // continuously fetch from disk, evict dirty frames, and maintain slotted page integrity.
    std::vector<RID> all_rids;
    all_rids.reserve(inserted_records.size());
    for (const auto &pair : inserted_records) {
        all_rids.push_back(pair.first);
    }

    std::mt19937 g(12345);
    std::shuffle(all_rids.begin(), all_rids.end(), g);

    for (const auto &rid : all_rids) {
        Page *page = bpm->FetchPage(rid.GetPageId());
        ASSERT_NE(page, nullptr) << "Fetch failed for page " << rid.GetPageId();

        auto *table_page = reinterpret_cast<TablePage *>(page);
        Tuple retrieved;
        ASSERT_TRUE(table_page->GetTuple(rid, &retrieved))
            << "GetTuple failed for " << rid.ToString();

        std::string actual = TupleToString(retrieved);
        EXPECT_EQ(actual, inserted_records[rid])
            << "Data mismatch at " << rid.ToString();

        bpm->UnpinPage(rid.GetPageId(), false);
    }
}

// ============================================================================
// Test 2: TableHeap Multi-Page Sequential Scan Under Tight Buffer Frames
// ============================================================================
TEST_F(EndToEndStorageIntegrationTest, TableHeapScanUnderTightBuffer) {
    constexpr size_t BUFFER_POOL_SIZE = 4; // Tiny buffer pool
    constexpr size_t NUM_TUPLES = 400;

    auto disk_manager = std::make_unique<DiskManager>(test_db_);
    auto bpm = std::make_unique<BufferPoolManager>(BUFFER_POOL_SIZE, disk_manager.get());

    auto table_heap = std::make_unique<TableHeap>(bpm.get());
    std::vector<std::string> expected_data;
    std::vector<RID> inserted_rids;

    // 1. Insert 400 tuples through TableHeap.
    // Each tuple is ~150 bytes -> ~20 tuples per 4096B page -> ~20 pages created!
    // With only 4 buffer frames, pages are constantly evicted and linked via NextPageId.
    for (size_t i = 0; i < NUM_TUPLES; ++i) {
        std::string val = "Record_#" + std::to_string(i) + "_Padding_" + std::string(120, '*');
        Tuple tuple = CreatePayloadTuple(val);
        RID rid;
        ASSERT_TRUE(table_heap->InsertTuple(tuple, &rid)) << "Failed to insert tuple " << i;

        expected_data.push_back(val);
        inserted_rids.push_back(rid);
    }

    // 2. Scan entire heap using TableIterator.
    // TableIterator must fetch pages across boundaries and properly unpin previous pages,
    // otherwise a 4-frame pool would deadlock or fail.
    size_t count = 0;
    for (auto it = table_heap->Begin(); it != table_heap->End(); ++it) {
        ASSERT_LT(count, NUM_TUPLES);
        std::string actual = TupleToString(*it);
        EXPECT_EQ(actual, expected_data[count]);
        ++count;
    }
    EXPECT_EQ(count, NUM_TUPLES);

    // 3. Point lookups using GetTuple across all pages
    for (size_t i = 0; i < NUM_TUPLES; i += 7) {
        Tuple result;
        ASSERT_TRUE(table_heap->GetTuple(inserted_rids[i], &result));
        EXPECT_EQ(TupleToString(result), expected_data[i]);
    }
}

// ============================================================================
// Test 3: Random In-Place & Growth Updates with Continuous Evictions
// ============================================================================
TEST_F(EndToEndStorageIntegrationTest, RandomUpdatesAndCompactionUnderEviction) {
    constexpr size_t BUFFER_POOL_SIZE = 5;
    constexpr size_t NUM_TUPLES = 200;

    auto disk_manager = std::make_unique<DiskManager>(test_db_);
    auto bpm = std::make_unique<BufferPoolManager>(BUFFER_POOL_SIZE, disk_manager.get());
    auto table_heap = std::make_unique<TableHeap>(bpm.get());

    std::vector<RID> rids;
    std::vector<std::string> contents;

    // Insert 200 initial records
    for (size_t i = 0; i < NUM_TUPLES; ++i) {
        std::string initial = "Init_" + std::to_string(i) + "_Text_" + std::string(60, '0' + (i % 10));
        Tuple tuple = CreatePayloadTuple(initial);
        RID rid;
        ASSERT_TRUE(table_heap->InsertTuple(tuple, &rid));
        rids.push_back(rid);
        contents.push_back(initial);
    }

    // Update 80 records randomly (some shrink, some grow in-place, some trigger migration)
    std::mt19937 rng(42);
    for (size_t step = 0; step < 80; ++step) {
        size_t idx = rng() % NUM_TUPLES;
        // Make some tuples grow significantly
        std::string updated = "UPDATED_" + std::to_string(idx) + "_Step_" + std::to_string(step) +
                              "_" + std::string(80 + (step % 50), 'U');
        Tuple new_tuple = CreatePayloadTuple(updated);
        RID new_rid;

        ASSERT_TRUE(table_heap->UpdateTuple(new_tuple, rids[idx], &new_rid));
        rids[idx] = new_rid; // Update RID tracking (handles potential cross-page migration)
        contents[idx] = updated;
    }

    // Verify all 200 records reflect the current state
    for (size_t i = 0; i < NUM_TUPLES; ++i) {
        Tuple res;
        ASSERT_TRUE(table_heap->GetTuple(rids[i], &res)) << "Lookup failed for index " << i;
        EXPECT_EQ(TupleToString(res), contents[i]);
    }
}

// ============================================================================
// Test 4: Crash-Restart Persistence Across Massive Eviction
// ============================================================================
TEST_F(EndToEndStorageIntegrationTest, FullCrashRestartPersistenceAcrossEvictions) {
    constexpr size_t BUFFER_POOL_SIZE = 4;
    constexpr size_t NUM_RECORDS = 300;

    page_id_t first_page_id = INVALID_PAGE_ID;
    std::vector<std::pair<RID, std::string>> saved_entries;

    {
        // Session 1: Create TableHeap with tiny buffer pool and write 300 records
        auto disk_manager = std::make_unique<DiskManager>(test_db_);
        auto bpm = std::make_unique<BufferPoolManager>(BUFFER_POOL_SIZE, disk_manager.get());
        auto table_heap = std::make_unique<TableHeap>(bpm.get());
        first_page_id = table_heap->GetFirstPageId();

        for (size_t i = 0; i < NUM_RECORDS; ++i) {
            std::string text = "Persistent_Record_#" + std::to_string(i) + "_Payload_" +
                               std::string(100, static_cast<char>('A' + (i % 26)));
            Tuple tuple = CreatePayloadTuple(text);
            RID rid;
            ASSERT_TRUE(table_heap->InsertTuple(tuple, &rid));
            saved_entries.emplace_back(rid, text);
        }

        // Flush everything to disk
        bpm->FlushAllPages();
    } // Both BPM and DiskManager are destroyed here (simulating application crash/shutdown)

    {
        // Session 2: Reopen from disk with a fresh BufferPoolManager
        auto disk_manager = std::make_unique<DiskManager>(test_db_);
        auto bpm = std::make_unique<BufferPoolManager>(BUFFER_POOL_SIZE, disk_manager.get());
        auto table_heap = std::make_unique<TableHeap>(bpm.get(), first_page_id);

        // 1. Verify point lookups for all 300 records
        for (size_t i = 0; i < saved_entries.size(); ++i) {
            Tuple retrieved;
            ASSERT_TRUE(table_heap->GetTuple(saved_entries[i].first, &retrieved))
                << "Failed to fetch persisted tuple at " << saved_entries[i].first.ToString();
            EXPECT_EQ(TupleToString(retrieved), saved_entries[i].second);
        }

        // 2. Verify TableIterator scan across all persisted pages
        size_t scan_count = 0;
        for (auto it = table_heap->Begin(); it != table_heap->End(); ++it) {
            ASSERT_LT(scan_count, saved_entries.size());
            EXPECT_EQ(TupleToString(*it), saved_entries[scan_count].second);
            ++scan_count;
        }
        EXPECT_EQ(scan_count, NUM_RECORDS);
    }
}
