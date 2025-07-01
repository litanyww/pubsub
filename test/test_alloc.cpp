#include "alloc.h"

#include <gtest/gtest.h>

#include <mutex>

TEST(Alloc, Pool)
{
    tbd::Pool<100> pool{nullptr, 32, 4};

    std::mutex mutex{};
    std::scoped_lock<std::mutex> guard{mutex};

    ASSERT_EQ(0U, pool.GetUseCount());
    ASSERT_FALSE(pool.IsExhausted());

    void* address = pool.Allocate(guard);
    ASSERT_NE(nullptr, address);
    ASSERT_EQ(1U, pool.GetUseCount());
    ASSERT_FALSE(pool.IsExhausted());

    pool.Free(guard, address);
    ASSERT_EQ(0U, pool.GetUseCount());
    ASSERT_FALSE(pool.IsExhausted());

    void* second = pool.Allocate(guard);
    ASSERT_EQ(second, address);

    void* third = pool.Allocate(guard);
    ASSERT_NE(third, second);
    ASSERT_EQ(2U, pool.GetUseCount());
    ASSERT_TRUE(pool.IsExhausted());
}

TEST(Alloc, AllPools)
{
    tbd::AllPools<1024UL> pools{};

    void* address = pools.GetPool(sizeof(int), alignof(int)).Allocate(4UL, 4UL);
    ASSERT_NE(nullptr, address);

    ASSERT_EQ(1U, tbd::Pool<1024UL>::GetPool(address)->GetUseCount());

    pools.GetPool(sizeof(int), alignof(int)).Free(address);

    ASSERT_EQ(0U, tbd::Pool<1024UL>::GetPool(address)->GetUseCount());

    void* second = pools.GetPool(sizeof(int), alignof(int)).Allocate(4UL, 4UL);
    ASSERT_EQ(second, address);

    void* third = pools.GetPool(sizeof(int), alignof(int)).Allocate(4UL, 4UL);
    ASSERT_NE(third, second);
    ASSERT_EQ(2U, tbd::Pool<1024UL>::GetPool(address)->GetUseCount());
}

TEST(Alloc, Map)
{
    std::map<int, int, std::less<int>, tbd::Allocator<std::pair<const int, int>>> map{};
    map[1] = 2;
}