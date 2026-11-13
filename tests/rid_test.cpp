#include <gtest/gtest.h>
#include <algorithm>
#include <map>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "rid.h"

class RIDTest : public ::testing::Test {};

TEST_F(RIDTest, DefaultConstructor) {
    RID rid;
    EXPECT_EQ(rid.GetPageId(), INVALID_PAGE_ID);
    EXPECT_EQ(rid.GetSlotNum(), 0);
    EXPECT_FALSE(rid.IsValid());
    EXPECT_EQ(rid.ToString(), "[-1, 0]");
}

TEST_F(RIDTest, ParameterizedConstructor) {
    RID rid(42, 15);
    EXPECT_EQ(rid.GetPageId(), 42);
    EXPECT_EQ(rid.GetSlotNum(), 15);
    EXPECT_TRUE(rid.IsValid());
    EXPECT_EQ(rid.ToString(), "[42, 15]");
}

TEST_F(RIDTest, SettersAndReset) {
    RID rid;
    EXPECT_FALSE(rid.IsValid());

    rid.Set(100, 20);
    EXPECT_EQ(rid.GetPageId(), 100);
    EXPECT_EQ(rid.GetSlotNum(), 20);
    EXPECT_TRUE(rid.IsValid());

    rid.SetPageId(200);
    EXPECT_EQ(rid.GetPageId(), 200);

    rid.SetSlotNum(30);
    EXPECT_EQ(rid.GetSlotNum(), 30);

    rid.Reset();
    EXPECT_EQ(rid.GetPageId(), INVALID_PAGE_ID);
    EXPECT_EQ(rid.GetSlotNum(), 0);
    EXPECT_FALSE(rid.IsValid());
}

TEST_F(RIDTest, Int64PackingRoundTrip) {
    std::vector<std::pair<page_id_t, uint32_t>> test_cases = {
        {0, 0},
        {1, 0},
        {0, 1},
        {42, 100},
        {12345, 6789},
        {1000000, 50000},
        {INT32_MAX, UINT32_MAX},
        {INVALID_PAGE_ID, 0},
        {INVALID_PAGE_ID, 9999}
    };

    for (const auto &[page_id, slot_num] : test_cases) {
        RID original(page_id, slot_num);
        int64_t packed = original.Get();
        EXPECT_EQ(packed, original.ToInt64());

        RID unpacked(packed);
        EXPECT_EQ(unpacked.GetPageId(), page_id);
        EXPECT_EQ(unpacked.GetSlotNum(), slot_num);
        EXPECT_EQ(unpacked, original);

        RID factory = RID::FromInt64(packed);
        EXPECT_EQ(factory, original);
    }
}

TEST_F(RIDTest, EqualityAndInequality) {
    RID r1(1, 2);
    RID r2(1, 2);
    RID r3(1, 3);
    RID r4(2, 2);

    EXPECT_EQ(r1, r2);
    EXPECT_NE(r1, r3);
    EXPECT_NE(r1, r4);
    EXPECT_NE(r3, r4);
}

TEST_F(RIDTest, ComparisonAndSorting) {
    RID r_1_1(1, 1);
    RID r_1_2(1, 2);
    RID r_2_0(2, 0);
    RID r_2_1(2, 1);

    EXPECT_LT(r_1_1, r_1_2);
    EXPECT_LE(r_1_1, r_1_2);
    EXPECT_LE(r_1_1, r_1_1);

    EXPECT_LT(r_1_2, r_2_0);
    EXPECT_LT(r_2_0, r_2_1);

    EXPECT_GT(r_2_1, r_2_0);
    EXPECT_GE(r_2_1, r_2_0);
    EXPECT_GE(r_2_1, r_2_1);

    // Verify sorting order
    std::vector<RID> rids = {r_2_1, r_1_2, r_2_0, r_1_1};
    std::sort(rids.begin(), rids.end());

    std::vector<RID> expected = {r_1_1, r_1_2, r_2_0, r_2_1};
    EXPECT_EQ(rids, expected);
}

TEST_F(RIDTest, OrderedMapContainer) {
    std::map<RID, std::string> rid_map;
    rid_map[RID(5, 2)] = "Row5_2";
    rid_map[RID(1, 0)] = "Row1_0";
    rid_map[RID(3, 7)] = "Row3_7";

    EXPECT_EQ(rid_map.size(), 3);
    EXPECT_EQ(rid_map[RID(1, 0)], "Row1_0");
    EXPECT_EQ(rid_map[RID(3, 7)], "Row3_7");
    EXPECT_EQ(rid_map[RID(5, 2)], "Row5_2");

    // Map keys should be ordered ascending by RID
    auto it = rid_map.begin();
    EXPECT_EQ(it->first, RID(1, 0));
    ++it;
    EXPECT_EQ(it->first, RID(3, 7));
    ++it;
    EXPECT_EQ(it->first, RID(5, 2));
}

TEST_F(RIDTest, UnorderedContainersAndHashing) {
    std::unordered_set<RID> rid_set;
    rid_set.insert(RID(10, 1));
    rid_set.insert(RID(20, 2));
    rid_set.insert(RID(10, 1)); // Duplicate

    EXPECT_EQ(rid_set.size(), 2);
    EXPECT_TRUE(rid_set.contains(RID(10, 1)));
    EXPECT_TRUE(rid_set.contains(RID(20, 2)));
    EXPECT_FALSE(rid_set.contains(RID(30, 3)));

    std::unordered_map<RID, int> rid_map;
    rid_map[RID(1, 1)] = 100;
    rid_map[RID(2, 2)] = 200;

    EXPECT_EQ(rid_map[RID(1, 1)], 100);
    EXPECT_EQ(rid_map[RID(2, 2)], 200);
}

TEST_F(RIDTest, StreamOutput) {
    RID rid(77, 3);
    std::stringstream ss;
    ss << rid;
    EXPECT_EQ(ss.str(), "[77, 3]");
}

TEST_F(RIDTest, EncodingBoundaryValues) {
    RID rid1(-1, 0);
    RID rid2(INT32_MAX, UINT32_MAX);
    RID rid3(0, UINT32_MAX);
    RID rid4(INT32_MIN, 0);

    EXPECT_EQ(RID(rid1.Get()), rid1);
    EXPECT_EQ(RID(rid2.Get()), rid2);
    EXPECT_EQ(RID(rid3.Get()), rid3);
    EXPECT_EQ(RID(rid4.Get()), rid4);
    EXPECT_NE(rid1.Get(), rid2.Get());
    EXPECT_NE(rid1.Get(), rid3.Get());
}
