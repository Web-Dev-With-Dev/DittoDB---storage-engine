#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <algorithm>
#include "generic_key.h"
#include "b_plus_tree_leaf_page.h"
#include "b_plus_tree_internal_page.h"

class GenericKeyTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// ============================================================================
// Test 1: Standard Layout and Trivially Copyable Guarantees
// ============================================================================
TEST_F(GenericKeyTest, LayoutAndSizing) {
    EXPECT_EQ(sizeof(GenericKey<4>), 4);
    EXPECT_EQ(sizeof(GenericKey<8>), 8);
    EXPECT_EQ(sizeof(GenericKey<16>), 16);
    EXPECT_EQ(sizeof(GenericKey<64>), 64);

    EXPECT_TRUE(std::is_standard_layout_v<GenericKey<4>>);
    EXPECT_TRUE(std::is_standard_layout_v<GenericKey<8>>);
    EXPECT_TRUE(std::is_standard_layout_v<GenericKey<16>>);
    EXPECT_TRUE(std::is_standard_layout_v<GenericKey<64>>);

    EXPECT_TRUE(std::is_trivially_copyable_v<GenericKey<4>>);
    EXPECT_TRUE(std::is_trivially_copyable_v<GenericKey<8>>);
    EXPECT_TRUE(std::is_trivially_copyable_v<GenericKey<16>>);
    EXPECT_TRUE(std::is_trivially_copyable_v<GenericKey<64>>);
}

// ============================================================================
// Test 2: Unsigned Integer Serialization & Order Preservation
// ============================================================================
TEST_F(GenericKeyTest, UnsignedIntegerSerialization) {
    std::vector<uint32_t> vals = {0, 1, 10, 255, 1000, 65535, 1000000, 0xFFFFFFFFu};
    std::vector<GenericKey<4>> keys;

    for (uint32_t v : vals) {
        auto key = GenericKey<4>::From(v);
        EXPECT_EQ(key.CastTo<uint32_t>(), v);
        keys.push_back(key);
    }

    // Verify raw byte ordering matches numerical ordering
    for (size_t i = 1; i < keys.size(); ++i) {
        EXPECT_TRUE(keys[i - 1] < keys[i]);
        EXPECT_LE(keys[i - 1], keys[i]);
        EXPECT_FALSE(keys[i] < keys[i - 1]);
        EXPECT_EQ(std::memcmp(keys[i - 1].GetData(), keys[i].GetData(), 4) < 0, true);
    }

    // 64-bit unsigned
    std::vector<uint64_t> vals64 = {0, 100, 10000000000ULL, 0xFFFFFFFFFFFFFFFFULL};
    std::vector<GenericKey<8>> keys64;
    for (uint64_t v : vals64) {
        auto key = GenericKey<8>::From(v);
        EXPECT_EQ(key.CastTo<uint64_t>(), v);
        keys64.push_back(key);
    }
    for (size_t i = 1; i < keys64.size(); ++i) {
        EXPECT_TRUE(keys64[i - 1] < keys64[i]);
    }
}

// ============================================================================
// Test 3: Signed Integer Serialization (Sign-Bit Inversion Order Preservation)
// ============================================================================
TEST_F(GenericKeyTest, SignedIntegerOrderPreservingSerialization) {
    std::vector<int32_t> vals = {
        -2147483647 - 1, -1000000, -500, -10, -1, 0, 1, 10, 500, 1000000, 2147483647
    };
    std::vector<GenericKey<4>> keys;

    for (int32_t v : vals) {
        auto key = GenericKey<4>::From(v);
        EXPECT_EQ(key.CastTo<int32_t>(), v);
        keys.push_back(key);
    }

    // Crucial Invariant: Even for negative numbers, raw byte memcmp matches numerical order!
    for (size_t i = 1; i < keys.size(); ++i) {
        EXPECT_TRUE(keys[i - 1] < keys[i]);
        EXPECT_EQ(std::memcmp(keys[i - 1].GetData(), keys[i].GetData(), 4) < 0, true);
    }

    // 64-bit signed
    std::vector<int64_t> vals64 = {-9223372036854775807LL - 1, -1000, -1, 0, 1, 1000, 9223372036854775807LL};
    std::vector<GenericKey<8>> keys64;
    for (int64_t v : vals64) {
        auto key = GenericKey<8>::From(v);
        EXPECT_EQ(key.CastTo<int64_t>(), v);
        keys64.push_back(key);
    }
    for (size_t i = 1; i < keys64.size(); ++i) {
        EXPECT_TRUE(keys64[i - 1] < keys64[i]);
        EXPECT_EQ(std::memcmp(keys64[i - 1].GetData(), keys64[i].GetData(), 8) < 0, true);
    }
}

// ============================================================================
// Test 4: Floating Point Serialization & Order Preservation
// ============================================================================
TEST_F(GenericKeyTest, FloatingPointSerialization) {
    std::vector<float> floats = {-1000.5f, -10.0f, -0.5f, 0.0f, 0.5f, 10.0f, 1000.5f};
    std::vector<GenericKey<4>> keys;

    for (float f : floats) {
        auto key = GenericKey<4>::From(f);
        EXPECT_FLOAT_EQ(key.CastTo<float>(), f);
        keys.push_back(key);
    }

    for (size_t i = 1; i < keys.size(); ++i) {
        EXPECT_TRUE(keys[i - 1] < keys[i]);
    }

    // Doubles
    std::vector<double> doubles = {-99999.99, -1.0, 0.0, 3.14159, 99999.99};
    std::vector<GenericKey<8>> dkeys;
    for (double d : doubles) {
        auto key = GenericKey<8>::From(d);
        EXPECT_DOUBLE_EQ(key.CastTo<double>(), d);
        dkeys.push_back(key);
    }
    for (size_t i = 1; i < dkeys.size(); ++i) {
        EXPECT_TRUE(dkeys[i - 1] < dkeys[i]);
    }
}

// ============================================================================
// Test 5: String Key Serialization & Lexicographical Ordering
// ============================================================================
TEST_F(GenericKeyTest, StringKeySerialization) {
    std::vector<std::string> words = {"alpha", "bravo", "charlie", "delta", "echo", "foxtrot"};
    std::vector<GenericKey<16>> keys;

    for (const auto &w : words) {
        auto key = GenericKey<16>::From(w);
        EXPECT_EQ(key.AsStringView(), w);
        keys.push_back(key);
    }

    for (size_t i = 1; i < keys.size(); ++i) {
        EXPECT_TRUE(keys[i - 1] < keys[i]);
    }

    // Test truncation on overflow
    std::string long_str = "this_string_is_much_too_long_for_sixteen_bytes";
    auto truncated_key = GenericKey<16>::From(long_str);
    EXPECT_EQ(truncated_key.AsStringView().size(), 16);
    EXPECT_EQ(truncated_key.AsStringView(), "this_string_is_m");
}

// ============================================================================
// Test 6: GenericComparator and KeyComparator
// ============================================================================
TEST_F(GenericKeyTest, Comparators) {
    GenericComparator<8> gen_comp;
    auto k1 = GenericKey<8>::From(100LL);
    auto k2 = GenericKey<8>::From(200LL);
    auto k3 = GenericKey<8>::From(100LL);

    EXPECT_LT(gen_comp(k1, k2), 0);
    EXPECT_GT(gen_comp(k2, k1), 0);
    EXPECT_EQ(gen_comp(k1, k3), 0);

    // Universal KeyComparator template
    KeyComparator<GenericKey<8>> key_comp;
    EXPECT_LT(key_comp(k1, k2), 0);
    EXPECT_GT(key_comp(k2, k1), 0);
    EXPECT_EQ(key_comp(k1, k3), 0);

    KeyComparator<int> int_comp;
    EXPECT_LT(int_comp(10, 20), 0);
    EXPECT_GT(int_comp(20, 10), 0);
    EXPECT_EQ(int_comp(15, 15), 0);
}

// ============================================================================
// Test 7: Max-Size Computation from Page Size
// ============================================================================
TEST_F(GenericKeyTest, PageSizeEstimator) {
    // 1. Internal page capacity: (4096 - 24) / entry_size
    // int (4B) + page_id_t (4B) = 8B -> 509 entries
    int internal_cap_int = BPlusTreePageSizeEstimator::ComputeInternalMaxSize<int, page_id_t>();
    EXPECT_EQ(internal_cap_int, 509);

    // GenericKey<8> (8B) + page_id_t (4B) = 12B -> (4096 - 24) / 12 = 339 entries
    int internal_cap_k8 = BPlusTreePageSizeEstimator::ComputeInternalMaxSize<GenericKey<8>, page_id_t>();
    EXPECT_EQ(internal_cap_k8, (4096 - 24) / sizeof(std::pair<GenericKey<8>, page_id_t>));

    // GenericKey<16> (16B) + page_id_t (4B) = 20B -> (4096 - 24) / 20 = 203 entries
    int internal_cap_k16 = BPlusTreePageSizeEstimator::ComputeInternalMaxSize<GenericKey<16>, page_id_t>();
    EXPECT_EQ(internal_cap_k16, (4096 - 24) / sizeof(std::pair<GenericKey<16>, page_id_t>));

    // 2. Leaf page capacity: (4096 - 32) / entry_size
    // int (4B) + RID (8B) = 12B -> 338 entries
    int leaf_cap_int = BPlusTreePageSizeEstimator::ComputeLeafMaxSize<int, RID>();
    EXPECT_EQ(leaf_cap_int, 338);

    // GenericKey<8> (8B) + RID (8B) = 16B -> (4096 - 32) / 16 = 254 entries
    int leaf_cap_k8 = BPlusTreePageSizeEstimator::ComputeLeafMaxSize<GenericKey<8>, RID>();
    EXPECT_EQ(leaf_cap_k8, 254);

    // GenericKey<64> (64B) + RID (8B) = 72B -> (4096 - 32) / 72 = 56 entries
    int leaf_cap_k64 = BPlusTreePageSizeEstimator::ComputeLeafMaxSize<GenericKey<64>, RID>();
    EXPECT_EQ(leaf_cap_k64, 56);
}

// ============================================================================
// Test 8: BPlusTreeLeafPage Instantiated with GenericKey & GenericComparator
// ============================================================================
TEST_F(GenericKeyTest, LeafPageWithGenericKey) {
    using LeafKey = GenericKey<8>;
    using TestLeafPage = BPlusTreeLeafPage<LeafKey, RID, GenericComparator<8>>;

    EXPECT_LE(sizeof(TestLeafPage), PAGE_SIZE);

    TestLeafPage leaf;
    leaf.Init(1, INVALID_PAGE_ID, 10);

    // Insert signed integers via GenericKey in arbitrary order
    std::vector<int64_t> raw_keys = {5000, -200, 80000, -100000, 0, 250, -1};
    for (int64_t rk : raw_keys) {
        leaf.Insert(LeafKey::From(rk), RID(1, static_cast<uint32_t>(rk & 0xFFFF)));
    }

    EXPECT_EQ(leaf.GetSize(), 7);

    // Verify sorted ascending order across negative, zero, positive
    std::vector<int64_t> sorted_expected = {-100000, -200, -1, 0, 250, 5000, 80000};
    for (int i = 0; i < leaf.GetSize(); ++i) {
        EXPECT_EQ(leaf.KeyAt(i).CastTo<int64_t>(), sorted_expected[i]);
    }

    // Lookup
    RID found;
    EXPECT_TRUE(leaf.Lookup(LeafKey::From(0LL), &found));
    EXPECT_TRUE(leaf.Lookup(LeafKey::From(-100000LL), &found));
    EXPECT_FALSE(leaf.Lookup(LeafKey::From(99999LL)));
}

// ============================================================================
// Test 9: BPlusTreeInternalPage Instantiated with GenericKey & GenericComparator
// ============================================================================
TEST_F(GenericKeyTest, InternalPageWithGenericKey) {
    using InternalKey = GenericKey<8>;
    using TestInternalPage = BPlusTreeInternalPage<InternalKey, page_id_t, GenericComparator<8>>;

    EXPECT_LE(sizeof(TestInternalPage), PAGE_SIZE);

    TestInternalPage internal;
    internal.Init(1, INVALID_PAGE_ID, 10);

    internal.Populate(10, InternalKey::From(100LL), 20);
    internal.InsertNodeAfter(20, InternalKey::From(500LL), 30);
    internal.InsertNodeAfter(10, InternalKey::From(-50LL), 15);

    // Child routing:
    // Pointers: 10, 15, 20, 30
    // Keys:         -50, 100, 500
    EXPECT_EQ(internal.Lookup(InternalKey::From(-100LL)), 10);
    EXPECT_EQ(internal.Lookup(InternalKey::From(-50LL)), 15);
    EXPECT_EQ(internal.Lookup(InternalKey::From(0LL)), 15);
    EXPECT_EQ(internal.Lookup(InternalKey::From(100LL)), 20);
    EXPECT_EQ(internal.Lookup(InternalKey::From(250LL)), 20);
    EXPECT_EQ(internal.Lookup(InternalKey::From(500LL)), 30);
    EXPECT_EQ(internal.Lookup(InternalKey::From(1000LL)), 30);
}
