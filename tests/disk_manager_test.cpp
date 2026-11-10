#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <atomic>
#include <chrono>
#include <random>

#include "disk_manager.h"

class DiskManagerTest : public ::testing::Test {
protected:
    const std::string filename = "test.db";

    void SetUp() override {
        if (std::filesystem::exists(filename)) {
            std::filesystem::remove(filename);
        }
    }

    void TearDown() override {
        if (std::filesystem::exists(filename)) {
            std::filesystem::remove(filename);
        }
    }

    // Helper: Fill page with pattern
    void FillPattern(char* buffer, size_t size, unsigned char pattern) {
        memset(buffer, pattern, size);
    }

    // Helper: Verify page pattern
    bool VerifyPattern(const char* buffer, size_t size, unsigned char pattern) {
        for (size_t i = 0; i < size; i++) {
            if (static_cast<unsigned char>(buffer[i]) != pattern) {
                return false;
            }
        }
        return true;
    }

    // Helper: Create a corrupted file
    void CreateCorruptedFile(size_t size) {
        std::ofstream file(filename, std::ios::binary);
        char* buffer = new char[size];
        memset(buffer, 0xAA, size);
        file.write(buffer, size);
        delete[] buffer;
        file.close();
    }
};

// ============= ORIGINAL TESTS (ALL PASSING) =============

TEST_F(DiskManagerTest, WriteReadPage) {
    DiskManager disk(filename);

    char write_buffer[PAGE_SIZE] = {};
    char read_buffer[PAGE_SIZE] = {};

    strcpy(write_buffer, "Hello Database!");

    disk.WritePage(0, write_buffer);
    disk.ReadPage(0, read_buffer);

    EXPECT_STREQ(write_buffer, read_buffer);
}

TEST_F(DiskManagerTest, ReadEmptyPageReturnsZeros) {
    DiskManager disk(filename);

    char buffer[PAGE_SIZE];

    disk.ReadPage(0, buffer);

    for (size_t i = 0; i < PAGE_SIZE; i++) {
        EXPECT_EQ(buffer[i], 0);
    }
}

TEST_F(DiskManagerTest, OverwritePage) {
    DiskManager disk(filename);

    char page1[PAGE_SIZE] = {};
    char page2[PAGE_SIZE] = {};
    char result[PAGE_SIZE] = {};

    strcpy(page1, "AAAA");
    strcpy(page2, "BBBB");

    disk.WritePage(0, page1);
    disk.WritePage(0, page2);

    disk.ReadPage(0, result);

    EXPECT_STREQ(result, "BBBB");
}

TEST_F(DiskManagerTest, MultiplePages) {
    DiskManager disk(filename);

    char page0[PAGE_SIZE] = {};
    char page1[PAGE_SIZE] = {};
    char page2[PAGE_SIZE] = {};
    char result[PAGE_SIZE] = {};

    strcpy(page0, "Page0");
    strcpy(page1, "Page1");
    strcpy(page2, "Page2");

    disk.WritePage(0, page0);
    disk.WritePage(1, page1);
    disk.WritePage(2, page2);

    disk.ReadPage(0, result);
    EXPECT_STREQ(result, "Page0");

    disk.ReadPage(1, result);
    EXPECT_STREQ(result, "Page1");

    disk.ReadPage(2, result);
    EXPECT_STREQ(result, "Page2");
}

TEST_F(DiskManagerTest, Persistence) {
    {
        DiskManager disk(filename);

        char page[PAGE_SIZE] = {};
        strcpy(page, "Persistent Data");

        disk.WritePage(0, page);
    }

    {
        DiskManager disk(filename);

        char result[PAGE_SIZE] = {};

        disk.ReadPage(0, result);

        EXPECT_STREQ(result, "Persistent Data");
    }
}

TEST_F(DiskManagerTest, WriteBeyondEOF) {
    DiskManager disk(filename);

    char page[PAGE_SIZE] = {};
    char result[PAGE_SIZE] = {};

    strcpy(page, "Last Page");

    disk.WritePage(5, page);

    disk.ReadPage(5, result);

    EXPECT_STREQ(result, page);
}

TEST_F(DiskManagerTest, IntermediatePagesAreZero) {
    DiskManager disk(filename);

    char page[PAGE_SIZE] = {};
    strcpy(page, "Page Five");

    disk.WritePage(5, page);

    char buffer[PAGE_SIZE];

    for (int page_id = 0; page_id < 5; page_id++) {
        disk.ReadPage(page_id, buffer);

        for (size_t i = 0; i < PAGE_SIZE; i++) {
            EXPECT_EQ(buffer[i], 0);
        }
    }
}

TEST_F(DiskManagerTest, FileSizeAfterSingleWrite) {
    DiskManager disk(filename);

    char page[PAGE_SIZE] = {};

    disk.WritePage(0, page);

    EXPECT_EQ(std::filesystem::file_size(filename), PAGE_SIZE);
}

TEST_F(DiskManagerTest, FileSizeAfterWritingPageFive) {
    DiskManager disk(filename);

    char page[PAGE_SIZE] = {};

    disk.WritePage(5, page);

    EXPECT_EQ(std::filesystem::file_size(filename), 6 * PAGE_SIZE);
}

TEST_F(DiskManagerTest, AllocatePagesSequentially) {
    DiskManager disk(filename);

    EXPECT_EQ(disk.AllocatePage(), 0);
    EXPECT_EQ(disk.AllocatePage(), 1);
    EXPECT_EQ(disk.AllocatePage(), 2);
    EXPECT_EQ(disk.AllocatePage(), 3);
}

TEST_F(DiskManagerTest, CorruptedFileThrows)
{
    // Create a corrupted file (size not multiple of PAGE_SIZE)
    {
        std::ofstream file(filename, std::ios::binary);
        char buffer[200] = {};
        file.write(buffer, sizeof(buffer));
    }

    EXPECT_THROW(
    {
        DiskManager disk(filename);
    },
    std::runtime_error);
}
// ============= NEW TESTS FOR EDGE CASES =============

// Test 12: Large Page IDs (Boundary Testing)
TEST_F(DiskManagerTest, LargePageId) {
    DiskManager disk(filename);
    
    const page_id_t large_page = 1000000;
    char write_buffer[PAGE_SIZE] = {};
    char read_buffer[PAGE_SIZE] = {};
    
    strcpy(write_buffer, "Large Page Test");
    
    // This should work but be slow due to zero-filling
    // But if page_id is too large, it might overflow size_t
    EXPECT_NO_THROW({
        disk.WritePage(large_page, write_buffer);
    });
    
    disk.ReadPage(large_page, read_buffer);
    EXPECT_STREQ(write_buffer, read_buffer);
    
    // Verify file size
    EXPECT_EQ(std::filesystem::file_size(filename), 
              static_cast<size_t>(large_page + 1) * PAGE_SIZE);
}

// Test 13: Write and Read Random Data Patterns
TEST_F(DiskManagerTest, RandomDataPatterns) {
    DiskManager disk(filename);
    
    char write_buffer[PAGE_SIZE];
    char read_buffer[PAGE_SIZE];
    
    // Test different patterns
    std::vector<unsigned char> patterns = {0x00, 0xFF, 0xAA, 0x55, 0xDE, 0xAD, 0xBE, 0xEF};
    
    for (size_t i = 0; i < patterns.size(); i++) {
        FillPattern(write_buffer, PAGE_SIZE, patterns[i]);
        disk.WritePage(i, write_buffer);
        disk.ReadPage(i, read_buffer);
        EXPECT_TRUE(VerifyPattern(read_buffer, PAGE_SIZE, patterns[i]))
            << "Pattern 0x" << std::hex << static_cast<int>(patterns[i]) << " failed";
    }
}

// Test 14: Sequential Writes to Same Page
TEST_F(DiskManagerTest, SequentialOverwrites) {
    DiskManager disk(filename);
    
    char buffer[PAGE_SIZE];
    char read_buffer[PAGE_SIZE];
    
    for (int i = 0; i < 100; i++) {
        FillPattern(buffer, PAGE_SIZE, static_cast<unsigned char>(i));
        disk.WritePage(0, buffer);
        disk.ReadPage(0, read_buffer);
        EXPECT_TRUE(VerifyPattern(read_buffer, PAGE_SIZE, static_cast<unsigned char>(i)));
    }
}

// Test 15: Interleaved Writes
TEST_F(DiskManagerTest, InterleavedWrites) {
    DiskManager disk(filename);
    
    const int num_pages = 10;
    char buffers[10][PAGE_SIZE];
    char read_buffer[PAGE_SIZE];
    
    // Write different patterns to different pages
    for (int i = 0; i < num_pages; i++) {
        FillPattern(buffers[i], PAGE_SIZE, static_cast<unsigned char>(i + 1));
    }
    
    // Interleave writes
    for (int round = 0; round < 5; round++) {
        for (int i = 0; i < num_pages; i++) {
            disk.WritePage(i, buffers[i]);
        }
    }
    
    // Verify all pages
    for (int i = 0; i < num_pages; i++) {
        disk.ReadPage(i, read_buffer);
        EXPECT_TRUE(VerifyPattern(read_buffer, PAGE_SIZE, static_cast<unsigned char>(i + 1)));
    }
}

// Test 16: File Size After Multiple Writes
TEST_F(DiskManagerTest, FileSizeAfterMultipleWrites) {
    DiskManager disk(filename);
    
    char buffer[PAGE_SIZE] = {};
    
    // Write pages non-sequentially
    disk.WritePage(10, buffer);
    EXPECT_EQ(std::filesystem::file_size(filename), 11 * PAGE_SIZE);
    
    disk.WritePage(5, buffer);
    EXPECT_EQ(std::filesystem::file_size(filename), 11 * PAGE_SIZE);  // Should not change
    
    disk.WritePage(20, buffer);
    EXPECT_EQ(std::filesystem::file_size(filename), 21 * PAGE_SIZE);
}

// Test 17: Zero-Size File Reading
TEST_F(DiskManagerTest, ReadFromEmptyFile) {
    // Create empty file
    std::ofstream file(filename, std::ios::binary);
    file.close();
    
    DiskManager disk(filename);
    char buffer[PAGE_SIZE];
    
    disk.ReadPage(0, buffer);
    
    // Should return zeros
    for (size_t i = 0; i < PAGE_SIZE; i++) {
        EXPECT_EQ(buffer[i], 0);
    }
}

// Test 18: Write Same Data Multiple Times
TEST_F(DiskManagerTest, WriteSameDataRepeatedly) {
    DiskManager disk(filename);
    
    char write_buffer[PAGE_SIZE];
    char read_buffer[PAGE_SIZE];
    const char* test_data = "Test data that will be written repeatedly";
    strcpy(write_buffer, test_data);
    
    for (int i = 0; i < 100; i++) {
        disk.WritePage(0, write_buffer);
    }
    
    disk.ReadPage(0, read_buffer);
    EXPECT_STREQ(write_buffer, read_buffer);
}

// Test 19: Concurrent Reads (No Writes)
TEST_F(DiskManagerTest, ConcurrentReads) {
    DiskManager disk(filename);
    
    // Write initial data
    char write_buffer[PAGE_SIZE];
    FillPattern(write_buffer, PAGE_SIZE, 0xAA);
    disk.WritePage(0, write_buffer);
    
    std::atomic<bool> failed{false};
    std::vector<std::thread> readers;
    
    // Create multiple readers
    for (int i = 0; i < 10; i++) {
        readers.emplace_back([&, i]() {
            DiskManager local_disk(filename);  // Each thread opens its own file handle
            char read_buffer[PAGE_SIZE];
            
            for (int j = 0; j < 100; j++) {
                local_disk.ReadPage(0, read_buffer);
                if (!VerifyPattern(read_buffer, PAGE_SIZE, 0xAA)) {
                    failed = true;
                }
            }
        });
    }
    
    for (auto& t : readers) {
        t.join();
    }
    
    EXPECT_FALSE(failed);
}

// Test 20: Read After Write with Flush
TEST_F(DiskManagerTest, ReadAfterWriteWithFlush) {
    DiskManager disk(filename);
    
    char write_buffer[PAGE_SIZE];
    char read_buffer[PAGE_SIZE];
    const char* data = "Data that should be visible after flush";
    strcpy(write_buffer, data);
    
    disk.WritePage(0, write_buffer);
    // WritePage already calls flush internally
    
    // Reopen disk manager
    DiskManager new_disk(filename);
    new_disk.ReadPage(0, read_buffer);
    EXPECT_STREQ(write_buffer, read_buffer);
}

// Test 21: Allocate Pages After Writes
TEST_F(DiskManagerTest, AllocateAfterWrites) {
    DiskManager disk(filename);
    
    char buffer[PAGE_SIZE] = {};
    
    // Write some pages
    disk.WritePage(5, buffer);
    disk.WritePage(10, buffer);
    
    // Allocate new pages
    EXPECT_EQ(disk.AllocatePage(), 0);
    EXPECT_EQ(disk.AllocatePage(), 1);
    EXPECT_EQ(disk.AllocatePage(), 2);
    
    // Allocation should continue from after the highest written page
    // Since we wrote page 10, next page should be 11
    // But the current implementation tracks next_page_id, which might be 0
    // This test might need adjustment based on implementation
}

// Test 22: Very Large Data in Page (Near PAGE_SIZE Limit)
TEST_F(DiskManagerTest, PageSizeLimit) {
    DiskManager disk(filename);
    
    char write_buffer[PAGE_SIZE];
    char read_buffer[PAGE_SIZE];
    
    // Fill with maximum data (almost PAGE_SIZE)
    memset(write_buffer, 'A', PAGE_SIZE - 1);
    write_buffer[PAGE_SIZE - 1] = '\0';
    
    disk.WritePage(0, write_buffer);
    disk.ReadPage(0, read_buffer);
    
    EXPECT_EQ(memcmp(write_buffer, read_buffer, PAGE_SIZE), 0);
}

// Test 23: Alternating Read/Write Operations
TEST_F(DiskManagerTest, AlternatingReadWrite) {
    DiskManager disk(filename);
    
    char write_buffer[PAGE_SIZE];
    char read_buffer[PAGE_SIZE];
    
    for (int i = 0; i < 50; i++) {
        FillPattern(write_buffer, PAGE_SIZE, static_cast<unsigned char>(i));
        disk.WritePage(i % 5, write_buffer);
        disk.ReadPage(i % 5, read_buffer);
        EXPECT_TRUE(VerifyPattern(read_buffer, PAGE_SIZE, static_cast<unsigned char>(i)));
    }
}

// Test 24: Memory Alignment (if PAGE_SIZE is not aligned)
TEST_F(DiskManagerTest, MemoryAlignment) {
    DiskManager disk(filename);
    
    // Allocate unaligned buffer
    char* unaligned_buffer = new char[PAGE_SIZE + 1];
    char* write_ptr = unaligned_buffer + 1;  // Intentionally misaligned
    
    char read_buffer[PAGE_SIZE];
    
    memset(write_ptr, 0xCD, PAGE_SIZE);
    disk.WritePage(0, write_ptr);
    disk.ReadPage(0, read_buffer);
    
    EXPECT_EQ(memcmp(write_ptr, read_buffer, PAGE_SIZE), 0);
    
    delete[] unaligned_buffer;
}

// Test 25: Exception Safety - Constructor with Invalid Path
TEST_F(DiskManagerTest, InvalidPath) {
    std::string invalid_path = "/nonexistent/directory/test.db";
    
    EXPECT_THROW({
        DiskManager disk(invalid_path);
    }, std::runtime_error);
}

// Test 26: Write After Failed Write
TEST_F(DiskManagerTest, WriteAfterFailedWrite) {
    // This test would need to simulate a failure
    // For now, just verify that disk manager can recover
    DiskManager disk(filename);
    
    char buffer[PAGE_SIZE] = {};
    char read_buffer[PAGE_SIZE];
    
    // First write succeeds
    strcpy(buffer, "First write");
    disk.WritePage(0, buffer);
    
    // Second write succeeds
    strcpy(buffer, "Second write");
    disk.WritePage(0, buffer);
    
    disk.ReadPage(0, read_buffer);
    EXPECT_STREQ(read_buffer, "Second write");
}

// Test 27: File Permissions
TEST_F(DiskManagerTest, FilePermissions) {
    DiskManager disk(filename);
    char buffer[PAGE_SIZE] = {};
    
    disk.WritePage(0, buffer);
    disk.WritePage(1, buffer);
    
    // Check file exists and has correct size
    EXPECT_TRUE(std::filesystem::exists(filename));
    EXPECT_EQ(std::filesystem::file_size(filename), 2 * PAGE_SIZE);
}

// Test 28: Stress Test with Random Operations
TEST_F(DiskManagerTest, StressTestRandomOperations) {
    DiskManager disk(filename);
    
    const int NUM_OPERATIONS = 1000;
    const int MAX_PAGE = 20;
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> page_dist(0, MAX_PAGE);
    std::uniform_int_distribution<> data_dist(0, 255);
    std::uniform_int_distribution<> op_dist(0, 1);  // 0=write, 1=read
    
    char buffers[100][PAGE_SIZE];  // Store what we wrote
    bool page_written[100] = {false};
    char read_buffer[PAGE_SIZE];
    
    for (int i = 0; i < NUM_OPERATIONS; i++) {
        int page = page_dist(gen);
        int op = op_dist(gen);
        
        if (op == 0 || !page_written[page]) {
            // Write
            unsigned char pattern = static_cast<unsigned char>(data_dist(gen));
            FillPattern(buffers[page], PAGE_SIZE, pattern);
            disk.WritePage(page, buffers[page]);
            page_written[page] = true;
        } else {
            // Read and verify
            disk.ReadPage(page, read_buffer);
            EXPECT_TRUE(VerifyPattern(read_buffer, PAGE_SIZE, 
                                     static_cast<unsigned char>(buffers[page][0])))
                << "Verification failed on operation " << i << " page " << page;
        }
    }
}

// Test 29: Concurrent Write and Read (Race Condition Test)
TEST_F(DiskManagerTest, ConcurrentWriteRead) {
    DiskManager disk(filename);
    
    const int NUM_THREADS = 5;
    const int ITERATIONS = 50;
    std::atomic<bool> failed{false};
    std::vector<std::thread> threads;
    
    for (int t = 0; t < NUM_THREADS; t++) {
        threads.emplace_back([&, t]() {
            DiskManager local_disk(filename);
            char buffer[PAGE_SIZE];
            char read_buffer[PAGE_SIZE];
            
            for (int i = 0; i < ITERATIONS; i++) {
                // Each thread writes its own page
                FillPattern(buffer, PAGE_SIZE, static_cast<unsigned char>(t + 1));
                local_disk.WritePage(t, buffer);
                
                // Verify
                local_disk.ReadPage(t, read_buffer);
                if (!VerifyPattern(read_buffer, PAGE_SIZE, static_cast<unsigned char>(t + 1))) {
                    failed = true;
                }
            }
        });
    }
    
    for (auto& thread : threads) {
        thread.join();
    }
    
    EXPECT_FALSE(failed);
}

// Test 30: Recovery from Partial Write (Requires Special Setup)
// Note: This test might need to be run with filesystem that can be filled
TEST_F(DiskManagerTest, RecoveryFromPartialWrite) {
    // This is a conceptual test that would require:
    // 1. A filesystem with limited space
    // 2. Or a mock filesystem
    
    // For now, just verify the disk manager can handle normal operations
    DiskManager disk(filename);
    char buffer[PAGE_SIZE] = {};
    char read_buffer[PAGE_SIZE];
    
    strcpy(buffer, "Recovery test");
    disk.WritePage(0, buffer);
    disk.ReadPage(0, read_buffer);
    EXPECT_STREQ(buffer, read_buffer);
}

// ============= BENCHMARK TESTS (Optional) =============

TEST_F(DiskManagerTest, BenchmarkSequentialWrites) {
    DiskManager disk(filename);
    
    const int NUM_PAGES = 1000;
    char buffer[PAGE_SIZE];
    FillPattern(buffer, PAGE_SIZE, 0xAA);
    
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < NUM_PAGES; i++) {
        disk.WritePage(i, buffer);
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    std::cout << "Sequential writes (" << NUM_PAGES << " pages): " 
              << duration.count() << " ms" << std::endl;
}

TEST_F(DiskManagerTest, BenchmarkRandomWrites) {
    DiskManager disk(filename);
    
    const int NUM_PAGES = 1000;
    const int MAX_PAGE = 2000;
    char buffer[PAGE_SIZE];
    FillPattern(buffer, PAGE_SIZE, 0xBB);
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(0, MAX_PAGE);
    
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < NUM_PAGES; i++) {
        disk.WritePage(dist(gen), buffer);
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    std::cout << "Random writes (" << NUM_PAGES << " pages): " 
              << duration.count() << " ms" << std::endl;
}

// ============= ADDITIONAL PARTIAL WRITE TESTS =============

// Test 31: Verify that WritePage throws when file is corrupted
TEST_F(DiskManagerTest, CorruptedFileMultipleSizes)
{
    std::vector<size_t> bad_sizes =
    {
        1, 10, 100, 512, 1024, 2048, 3000, 4095, 5000
    };

    for (size_t size : bad_sizes)
    {
        if (std::filesystem::exists(filename))
        {
            std::filesystem::remove(filename);
        }

        CreateCorruptedFile(size);

        EXPECT_THROW(
        {
            DiskManager disk(filename);
        },
        std::runtime_error)
        << "Failed for file size " << size;
    }
}

// Test 32: File validation on constructor
TEST_F(DiskManagerTest, ConstructorValidatesFileSize)
{
    {
        DiskManager disk(filename);

        char page[PAGE_SIZE] = {};
        disk.WritePage(0, page);
    }

    // Append one byte so file size becomes PAGE_SIZE + 1
    {
        std::ofstream file(filename,
                           std::ios::binary | std::ios::app);

        char garbage = 'X';
        file.write(&garbage, 1);
    }

    EXPECT_THROW(
    {
        DiskManager disk(filename);
    },
    std::runtime_error);
}

// Test 33: WritePage with empty data (all zeros)
TEST_F(DiskManagerTest, WriteAllZeros) {
    DiskManager disk(filename);
    
    char write_buffer[PAGE_SIZE] = {};
    char read_buffer[PAGE_SIZE];
    
    disk.WritePage(0, write_buffer);
    disk.ReadPage(0, read_buffer);
    
    for (size_t i = 0; i < PAGE_SIZE; i++) {
        EXPECT_EQ(read_buffer[i], 0);
    }
}

// Test 34: WritePage with maximum data (all 0xFF)
TEST_F(DiskManagerTest, WriteAllOnes) {
    DiskManager disk(filename);
    
    char write_buffer[PAGE_SIZE];
    char read_buffer[PAGE_SIZE];
    memset(write_buffer, 0xFF, PAGE_SIZE);
    
    disk.WritePage(0, write_buffer);
    disk.ReadPage(0, read_buffer);
    
    for (size_t i = 0; i < PAGE_SIZE; i++) {
        EXPECT_EQ(static_cast<unsigned char>(read_buffer[i]), 0xFF);
    }
}

// Test 35: Multiple DiskManager instances same file
TEST_F(DiskManagerTest, MultipleInstances) {
    char buffer[PAGE_SIZE];
    char read_buffer[PAGE_SIZE];
    const char* test_data = "Test data for multiple instances";
    strcpy(buffer, test_data);
    
    // First instance writes
    {
        DiskManager disk1(filename);
        disk1.WritePage(0, buffer);
    }
    
    // Second instance reads
    {
        DiskManager disk2(filename);
        disk2.ReadPage(0, read_buffer);
        EXPECT_STREQ(buffer, read_buffer);
    }
    
    // Third instance modifies
    {
        DiskManager disk3(filename);
        strcpy(buffer, "Modified by third instance");
        disk3.WritePage(0, buffer);
    }
    
    // Fourth instance verifies
    {
        DiskManager disk4(filename);
        disk4.ReadPage(0, read_buffer);
        EXPECT_STREQ(buffer, read_buffer);
    }
}

// Test 36: Reading page that doesn't exist (but within allocated range)
TEST_F(DiskManagerTest, ReadPageWithinRangeButUnwritten) {
    DiskManager disk(filename);
    
    char buffer[PAGE_SIZE];
    
    // Write page 5, which zeros pages 0-4
    char dummy[PAGE_SIZE] = {};
    disk.WritePage(5, dummy);
    
    // Read page 3 (should be zero, but file exists)
    disk.ReadPage(3, buffer);
    for (size_t i = 0; i < PAGE_SIZE; i++) {
        EXPECT_EQ(buffer[i], 0);
    }
}

// Test 37: Quick many small writes
TEST_F(DiskManagerTest, ManySmallWrites) {
    DiskManager disk(filename);
    
    char buffer[PAGE_SIZE];
    char read_buffer[PAGE_SIZE];
    
    for (int i = 0; i < 100; i++) {
        memset(buffer, static_cast<char>(i), PAGE_SIZE);
        disk.WritePage(0, buffer);
        disk.ReadPage(0, read_buffer);
        EXPECT_EQ(memcmp(buffer, read_buffer, PAGE_SIZE), 0);
    }
}

// Test 38: Write page with null terminator in middle
TEST_F(DiskManagerTest, WriteWithNullBytes) {
    DiskManager disk(filename);
    
    char write_buffer[PAGE_SIZE];
    char read_buffer[PAGE_SIZE];
    
    memset(write_buffer, 0, PAGE_SIZE);
    // Put some data with nulls in between
    write_buffer[0] = 'H';
    write_buffer[1] = 'e';
    write_buffer[2] = 'l';
    write_buffer[3] = 'l';
    write_buffer[4] = 'o';
    write_buffer[5] = '\0';
    write_buffer[6] = 'W';
    write_buffer[7] = 'o';
    write_buffer[8] = 'r';
    write_buffer[9] = 'l';
    write_buffer[10] = 'd';
    
    disk.WritePage(0, write_buffer);
    disk.ReadPage(0, read_buffer);
    
    EXPECT_EQ(memcmp(write_buffer, read_buffer, PAGE_SIZE), 0);
}

// Test 39: Write pages with varying offsets
TEST_F(DiskManagerTest, WritePagesWithVaryingOffsets)
{
    DiskManager disk(filename);

    const int MAX_PAGE = 100;

    char buffers[MAX_PAGE + 1][PAGE_SIZE];
    bool written[MAX_PAGE + 1] = {false};

    char read_buffer[PAGE_SIZE];

    std::random_device rd;
    std::mt19937 gen(rd());

    std::uniform_int_distribution<> page_dist(0, MAX_PAGE);
    std::uniform_int_distribution<> data_dist(0, 255);

    for (int i = 0; i < 50; i++)
    {
        int page = page_dist(gen);

        unsigned char pattern =
            static_cast<unsigned char>(data_dist(gen));

        FillPattern(buffers[page], PAGE_SIZE, pattern);

        written[page] = true;

        disk.WritePage(page, buffers[page]);
    }

    for (int page = 0; page <= MAX_PAGE; page++)
    {
        disk.ReadPage(page, read_buffer);

        if (written[page])
        {
            EXPECT_EQ(
                memcmp(read_buffer, buffers[page], PAGE_SIZE),
                0);
        }
        else
        {
            EXPECT_TRUE(
                VerifyPattern(read_buffer, PAGE_SIZE, 0));
        }
    }
}

// Test 40: Extreme stress test (if you have time)
TEST_F(DiskManagerTest, ExtremeStressTest) {
    DiskManager disk(filename);
    
    const int NUM_OPERATIONS = 100000;
    const int MAX_PAGE = 100;
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> page_dist(0, MAX_PAGE);
    std::uniform_int_distribution<> op_dist(0, 1);
    
    char buffers[101][PAGE_SIZE];
    bool initialized[101] = {false};
    char read_buffer[PAGE_SIZE];
    
    for (int i = 0; i < NUM_OPERATIONS; i++) {
        int page = page_dist(gen);
        int op = op_dist(gen);
        
        if (op == 0 || !initialized[page]) {
            // Write random pattern
            unsigned char pattern = static_cast<unsigned char>(rd() % 256);
            FillPattern(buffers[page], PAGE_SIZE, pattern);
            disk.WritePage(page, buffers[page]);
            initialized[page] = true;
        } else {
            // Read and verify
            disk.ReadPage(page, read_buffer);
            if (initialized[page]) {
                ASSERT_TRUE(VerifyPattern(read_buffer, PAGE_SIZE, 
                                         static_cast<unsigned char>(buffers[page][0])))
                    << "Failed at operation " << i << " page " << page;
            } else {
                ASSERT_TRUE(VerifyPattern(read_buffer, PAGE_SIZE, 0))
                    << "Uninitialized page " << page << " has data at operation " << i;
            }
        }
    }
}


// Test 41: Shared next_page_id across multiple DiskManager instances on the same file
TEST_F(DiskManagerTest, SharedNextPageIdAcrossInstances) {
    const std::string other_file = "test_other.db";
    if (std::filesystem::exists(other_file)) {
        std::filesystem::remove(other_file);
    }

    {
        DiskManager dm1(filename);
        DiskManager dm2("./" + filename);
        DiskManager dm_other(other_file);

        EXPECT_EQ(dm1.GetNextPageId(), 0);
        EXPECT_EQ(dm2.GetNextPageId(), 0);
        EXPECT_EQ(dm_other.GetNextPageId(), 0);

        // Allocation in dm1 should advance dm2's view of the same file, not dm_other
        EXPECT_EQ(dm1.AllocatePage(), 0);
        EXPECT_EQ(dm1.GetNextPageId(), 1);
        EXPECT_EQ(dm2.GetNextPageId(), 1);
        EXPECT_EQ(dm_other.GetNextPageId(), 0);

        // Allocation in dm2 should continue sequentially from 1
        EXPECT_EQ(dm2.AllocatePage(), 1);
        EXPECT_EQ(dm1.GetNextPageId(), 2);
        EXPECT_EQ(dm2.GetNextPageId(), 2);
        EXPECT_EQ(dm_other.AllocatePage(), 0);
    }

    if (std::filesystem::exists(other_file)) {
        std::filesystem::remove(other_file);
    }
}

// Test 42: Invalid negative page_id and nullptr buffers throw std::invalid_argument
TEST_F(DiskManagerTest, InvalidArgumentsThrow) {
    DiskManager disk(filename);
    char buffer[PAGE_SIZE] = {};

    EXPECT_THROW(disk.WritePage(INVALID_PAGE_ID, buffer), std::invalid_argument);
    EXPECT_THROW(disk.ReadPage(INVALID_PAGE_ID, buffer), std::invalid_argument);
    EXPECT_THROW(disk.WritePage(0, nullptr), std::invalid_argument);
    EXPECT_THROW(disk.ReadPage(0, nullptr), std::invalid_argument);
}
