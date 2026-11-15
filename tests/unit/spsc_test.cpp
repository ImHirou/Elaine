#include <gtest/gtest.h>

#include <thread>

#include "base/spsc.hpp"

using namespace base;

TEST(SpscRing, FillDrain) {
    SpscRing<u32> q(8);
    for (u32 i = 0; i < 8; ++i) EXPECT_TRUE(q.push(i));
    EXPECT_FALSE(q.push(99));
    u32 v = 0;
    for (u32 i = 0; i < 8; ++i) {
        ASSERT_TRUE(q.pop(v));
        EXPECT_EQ(v, i);
    }
    EXPECT_FALSE(q.pop(v));
}

TEST(SpscRing, TwoThreads) {
    SpscRing<u64> q(1024);
    constexpr u64 kN = 2'000'000;
    std::thread prod([&] {
        for (u64 i = 0; i < kN;) {
            if (q.push(i))
                ++i;
            else
                std::this_thread::yield();
        }
    });
    u64 expect = 0, v = 0;
    while (expect < kN) {
        if (q.pop(v)) {
            ASSERT_EQ(v, expect);
            ++expect;
        } else {
            std::this_thread::yield();
        }
    }
    prod.join();
}

TEST(SpscBytes, RecordsSurviveWrapAround) {
    SpscBytes q(256);
    u32 seq = 0, expect = 0;
    for (int round = 0; round < 1000; ++round) {
        const u32 n = 1 + (round * 7) % 60;
        u8* w = q.alloc(n);
        ASSERT_NE(w, nullptr);
        for (u32 i = 0; i < n; ++i) w[i] = static_cast<u8>(seq + i);
        q.commit();
        q.publish();
        ++seq;

        const u8* r = nullptr;
        u32 len = 0;
        ASSERT_TRUE(q.front(r, len));
        ASSERT_EQ(len, n);
        for (u32 i = 0; i < n; ++i) ASSERT_EQ(r[i], static_cast<u8>(expect + i));
        q.pop(len);
        q.release();
        ++expect;
    }
}

TEST(SpscBytes, FullThenRecovers) {
    SpscBytes q(256);
    int pushed = 0;
    while (u8* w = q.alloc(24)) {
        w[0] = static_cast<u8>(pushed++);
        q.commit();
    }
    q.publish();
    EXPECT_GT(pushed, 4);
    EXPECT_EQ(q.alloc(24), nullptr);
    const u8* r;
    u32 n;
    for (int i = 0; i < pushed; ++i) {
        ASSERT_TRUE(q.front(r, n));
        EXPECT_EQ(r[0], static_cast<u8>(i));
        q.pop(n);
    }
    q.release();
    EXPECT_FALSE(q.front(r, n));
    EXPECT_NE(q.alloc(24), nullptr);
}

TEST(SpscBytes, TwoThreadsVariableRecords) {
    SpscBytes q(1 << 16);
    constexpr u32 kN = 500'000;
    std::thread prod([&] {
        for (u32 i = 0; i < kN;) {
            const u32 n = 4 + (i % 97);
            u8* w = q.alloc(n);
            if (!w) {
                q.publish();
                std::this_thread::yield();
                continue;
            }
            store_le<u32>(w, i);
            for (u32 k = 4; k < n; ++k) w[k] = static_cast<u8>(i + k);
            q.commit();
            if ((i & 15) == 0) q.publish();  // batched publishing, like the reactor does
            ++i;
        }
        q.publish();
    });
    u32 expect = 0;
    while (expect < kN) {
        const u8* r;
        u32 n;
        if (!q.front(r, n)) {
            q.release();
            std::this_thread::yield();
            continue;
        }
        ASSERT_EQ(n, 4 + (expect % 97));
        ASSERT_EQ(load_le<u32>(r), expect);
        for (u32 k = 4; k < n; ++k) ASSERT_EQ(r[k], static_cast<u8>(expect + k));
        q.pop(n);
        ++expect;
    }
    prod.join();
}
