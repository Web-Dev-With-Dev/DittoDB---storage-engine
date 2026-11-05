#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "b_plus_tree.h"
#include "buffer_pool_manager.h"
#include "disk_manager.h"
#include "rid.h"

namespace fs = std::filesystem;

struct BenchmarkResult {
    std::string benchmark_name;
    size_t op_count{0};
    double duration_ms{0.0};
    double ops_per_sec{0.0};
    double avg_latency_us{0.0};
    BufferPoolStats bp_stats;
};

void PrintHeader() {
    std::cout << "\n" << std::string(125, '=') << "\n";
    std::cout << std::left
              << std::setw(32) << "Benchmark Scenario"
              << std::right
              << std::setw(10) << "Ops"
              << std::setw(12) << "Time (ms)"
              << std::setw(14) << "Throughput"
              << std::setw(12) << "Avg Lat"
              << std::setw(12) << "Hit Ratio"
              << std::setw(11) << "Disk Reads"
              << std::setw(12) << "Disk Writes"
              << std::setw(10) << "Evictions"
              << "\n";
    std::cout << std::left
              << std::setw(32) << ""
              << std::right
              << std::setw(10) << ""
              << std::setw(12) << ""
              << std::setw(14) << "(ops/sec)"
              << std::setw(12) << "(us/op)"
              << std::setw(12) << "(%)"
              << std::setw(11) << "(pages)"
              << std::setw(12) << "(pages)"
              << std::setw(10) << "(frames)"
              << "\n";
    std::cout << std::string(125, '-') << "\n";
}

void PrintResult(const BenchmarkResult &res) {
    std::cout << std::left
              << std::setw(32) << res.benchmark_name
              << std::right
              << std::setw(10) << res.op_count
              << std::setw(12) << std::fixed << std::setprecision(2) << res.duration_ms
              << std::setw(14) << std::fixed << std::setprecision(0) << res.ops_per_sec
              << std::setw(12) << std::fixed << std::setprecision(2) << res.avg_latency_us
              << std::setw(11) << std::fixed << std::setprecision(1) << res.bp_stats.HitRatio() << "%"
              << std::setw(11) << res.bp_stats.disk_read_count
              << std::setw(12) << res.bp_stats.disk_write_count
              << std::setw(10) << res.bp_stats.evict_count
              << "\n";
}

void RunBenchmarkSuite(const std::string &regime_name, size_t pool_size, size_t num_keys) {
    std::string db_file = "bench_" + regime_name + ".db";
    if (fs::exists(db_file)) {
        fs::remove(db_file);
    }

    std::cout << "\n>>> REGIME: " << regime_name
              << " (Buffer Pool Size: " << pool_size << " frames, Data Scale: " << num_keys << " keys) <<<\n";

    auto disk_manager = std::make_unique<DiskManager>(db_file);
    auto bpm = std::make_unique<BufferPoolManager>(pool_size, disk_manager.get());
    BPlusTree<int, RID> tree("bench_tree", bpm.get(), std::less<int>{}, 32, 32);

    PrintHeader();

    // ------------------------------------------------------------------------
    // 1. Sequential Bulk Insert
    // ------------------------------------------------------------------------
    {
        bpm->ResetStats();
        auto start = std::chrono::high_resolution_clock::now();

        for (size_t i = 1; i <= num_keys; ++i) {
            int key = static_cast<int>(i);
            tree.Insert(key, RID(static_cast<page_id_t>(key / 100 + 1), static_cast<uint32_t>(key)));
        }

        auto end = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();

        BenchmarkResult res;
        res.benchmark_name = "1. Bulk Insert (Sequential)";
        res.op_count = num_keys;
        res.duration_ms = ms;
        res.ops_per_sec = (num_keys / (ms / 1000.0));
        res.avg_latency_us = (ms * 1000.0) / num_keys;
        res.bp_stats = bpm->GetStats();
        PrintResult(res);
    }

    // ------------------------------------------------------------------------
    // 2. Sequential Point Lookups (100% Hit)
    // ------------------------------------------------------------------------
    {
        bpm->ResetStats();
        auto start = std::chrono::high_resolution_clock::now();

        size_t found = 0;
        RID rid;
        for (size_t i = 1; i <= num_keys; ++i) {
            if (tree.GetValue(static_cast<int>(i), &rid)) {
                found++;
            }
        }
        (void)found;

        auto end = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();

        BenchmarkResult res;
        res.benchmark_name = "2. Point Lookup (Sequential Hit)";
        res.op_count = num_keys;
        res.duration_ms = ms;
        res.ops_per_sec = (num_keys / (ms / 1000.0));
        res.avg_latency_us = (ms * 1000.0) / num_keys;
        res.bp_stats = bpm->GetStats();
        PrintResult(res);
    }

    // ------------------------------------------------------------------------
    // 3. Random Point Lookups (100% Hit)
    // ------------------------------------------------------------------------
    {
        std::vector<int> rand_keys(num_keys);
        std::iota(rand_keys.begin(), rand_keys.end(), 1);
        std::mt19937 g(42);
        std::shuffle(rand_keys.begin(), rand_keys.end(), g);

        bpm->ResetStats();
        auto start = std::chrono::high_resolution_clock::now();

        size_t found = 0;
        RID rid;
        for (int key : rand_keys) {
            if (tree.GetValue(key, &rid)) {
                found++;
            }
        }
        (void)found;

        auto end = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();

        BenchmarkResult res;
        res.benchmark_name = "3. Point Lookup (Random Hit)";
        res.op_count = num_keys;
        res.duration_ms = ms;
        res.ops_per_sec = (num_keys / (ms / 1000.0));
        res.avg_latency_us = (ms * 1000.0) / num_keys;
        res.bp_stats = bpm->GetStats();
        PrintResult(res);
    }

    // ------------------------------------------------------------------------
    // 4. Random Point Lookups (Misses / Non-existent keys)
    // ------------------------------------------------------------------------
    {
        size_t miss_ops = num_keys / 2;
        bpm->ResetStats();
        auto start = std::chrono::high_resolution_clock::now();

        size_t found = 0;
        RID rid;
        for (size_t i = 1; i <= miss_ops; ++i) {
            // Keys in [num_keys + 1, num_keys + 1 + miss_ops] do not exist
            int key = static_cast<int>(num_keys + i);
            if (tree.GetValue(key, &rid)) {
                found++;
            }
        }
        (void)found;

        auto end = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();

        BenchmarkResult res;
        res.benchmark_name = "4. Point Lookup (Miss/Absent)";
        res.op_count = miss_ops;
        res.duration_ms = ms;
        res.ops_per_sec = (miss_ops / (ms / 1000.0));
        res.avg_latency_us = (ms * 1000.0) / miss_ops;
        res.bp_stats = bpm->GetStats();
        PrintResult(res);
    }

    // ------------------------------------------------------------------------
    // 5. Short Range Scans (1,000 scans, length 10 items each)
    // ------------------------------------------------------------------------
    {
        const size_t scan_count = 1000;
        const int scan_len = 10;
        std::mt19937 rng(1337);
        std::uniform_int_distribution<int> dist(1, static_cast<int>(num_keys - scan_len));

        bpm->ResetStats();
        auto start = std::chrono::high_resolution_clock::now();

        size_t total_items_scanned = 0;
        std::vector<std::pair<int, RID>> scan_results;
        for (size_t s = 0; s < scan_count; ++s) {
            int start_key = dist(rng);
            int end_key = start_key + scan_len - 1;
            scan_results.clear();
            total_items_scanned += tree.RangeScan(start_key, end_key, &scan_results);
        }
        (void)total_items_scanned;

        auto end = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();

        BenchmarkResult res;
        res.benchmark_name = "5. Range Scan (Short: 10 items)";
        res.op_count = scan_count;
        res.duration_ms = ms;
        res.ops_per_sec = (scan_count / (ms / 1000.0));
        res.avg_latency_us = (ms * 1000.0) / scan_count;
        res.bp_stats = bpm->GetStats();
        PrintResult(res);
    }

    // ------------------------------------------------------------------------
    // 6. Medium Range Scans (200 scans, length 200 items each)
    // ------------------------------------------------------------------------
    {
        const size_t scan_count = 200;
        const int scan_len = 200;
        std::mt19937 rng(2024);
        std::uniform_int_distribution<int> dist(1, static_cast<int>(num_keys - scan_len));

        bpm->ResetStats();
        auto start = std::chrono::high_resolution_clock::now();

        size_t total_items_scanned = 0;
        std::vector<std::pair<int, RID>> scan_results;
        for (size_t s = 0; s < scan_count; ++s) {
            int start_key = dist(rng);
            int end_key = start_key + scan_len - 1;
            scan_results.clear();
            total_items_scanned += tree.RangeScan(start_key, end_key, &scan_results);
        }
        (void)total_items_scanned;

        auto end = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();

        BenchmarkResult res;
        res.benchmark_name = "6. Range Scan (Med: 200 items)";
        res.op_count = scan_count;
        res.duration_ms = ms;
        res.ops_per_sec = (scan_count / (ms / 1000.0));
        res.avg_latency_us = (ms * 1000.0) / scan_count;
        res.bp_stats = bpm->GetStats();
        PrintResult(res);
    }

    // ------------------------------------------------------------------------
    // 7. Full Table Scan (Sequential traversal from Begin() to End())
    // ------------------------------------------------------------------------
    {
        bpm->ResetStats();
        auto start = std::chrono::high_resolution_clock::now();

        size_t scanned_items = 0;
        for (auto it = tree.Begin(); it != tree.End(); ++it) {
            scanned_items++;
        }

        auto end = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();

        BenchmarkResult res;
        res.benchmark_name = "7. Full Table Scan (All Items)";
        res.op_count = scanned_items;
        res.duration_ms = ms;
        res.ops_per_sec = (scanned_items / (ms / 1000.0));
        res.avg_latency_us = (ms * 1000.0) / scanned_items;
        res.bp_stats = bpm->GetStats();
        PrintResult(res);
    }

    // ------------------------------------------------------------------------
    // 8. Random Bulk Insert (New Tree)
    // ------------------------------------------------------------------------
    {
        std::string rand_db_file = "bench_rand_" + regime_name + ".db";
        if (fs::exists(rand_db_file)) {
            fs::remove(rand_db_file);
        }

        auto rand_disk = std::make_unique<DiskManager>(rand_db_file);
        auto rand_bpm = std::make_unique<BufferPoolManager>(pool_size, rand_disk.get());
        BPlusTree<int, RID> rand_tree("bench_rand_tree", rand_bpm.get(), std::less<int>{}, 32, 32);

        std::vector<int> shuffled_keys(num_keys);
        std::iota(shuffled_keys.begin(), shuffled_keys.end(), 1);
        std::mt19937 g(777);
        std::shuffle(shuffled_keys.begin(), shuffled_keys.end(), g);

        rand_bpm->ResetStats();
        auto start = std::chrono::high_resolution_clock::now();

        for (int key : shuffled_keys) {
            rand_tree.Insert(key, RID(static_cast<page_id_t>(key / 100 + 1), static_cast<uint32_t>(key)));
        }

        auto end = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();

        BenchmarkResult res;
        res.benchmark_name = "8. Bulk Insert (Random)";
        res.op_count = num_keys;
        res.duration_ms = ms;
        res.ops_per_sec = (num_keys / (ms / 1000.0));
        res.avg_latency_us = (ms * 1000.0) / num_keys;
        res.bp_stats = rand_bpm->GetStats();
        PrintResult(res);

        rand_bpm.reset();
        rand_disk.reset();
        if (fs::exists(rand_db_file)) {
            fs::remove(rand_db_file);
        }
    }

    std::cout << std::string(125, '=') << "\n";

    bpm.reset();
    disk_manager.reset();
    if (fs::exists(db_file)) {
        fs::remove(db_file);
    }
}

int main() {
    std::cout << "\n============================================================================================\n";
    std::cout << "                 B+ TREE STORAGE ENGINE BENCHMARK SUITE                                    \n";
    std::cout << "         (Measuring Bulk Inserts, Point Lookups, Range Scans & Buffer Pool I/O)             \n";
    std::cout << "============================================================================================\n";

    // 1. In-Memory / Warm Cache (5,000 buffer frames — tree fits completely in memory)
    RunBenchmarkSuite("Warm_Cache_Large_BPM", 5000, 50000);

    // 2. Memory Constrained / High Eviction Pressure (64 buffer frames — active page replacement)
    RunBenchmarkSuite("Constrained_Cache_Small_BPM", 64, 25000);

    std::cout << "\nBenchmark suite completed successfully.\n\n";
    return 0;
}

