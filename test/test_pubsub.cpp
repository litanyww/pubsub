#include "pubsub.h"

#include <gtest/gtest.h>
#include <chrono>
#include <typeindex>
#include <latch>
#include <iostream>
#include <vector>
#include <string>
#include <future>
#include <deque>
#include <thread>

namespace
{
    using namespace std::chrono_literals;
    auto shortDelay = 10ms;

    struct TCompare
    {
        using is_transparent = void;

        template <class L, class R>
        bool operator()(const L &lhs, const R &rhs) const
        {
            bool result = lhs < rhs;
            // std::cerr << " " << lhs << " < " << rhs << " = " << std::boolalpha << result << "\n";
            return result;
        }
    };
    template <class T>
    bool show(std::pair<int, int> p, int trigger, int expectedFirst, int expectedLast)
    {
        std::map<T, int, TCompare> values{};
        for (int i = p.first; i <= p.second; ++i)
        {
            values.emplace(i, i);
        }
        auto [first, last] = values.equal_range(trigger);
        if (first->second != expectedFirst)
        {
            std::cerr << "first expected as " << expectedFirst << ", actual " << first->second << "\n";
            return false;
        }
        if ((--last)->second != expectedLast)
        {
            std::cerr << "second expected as " << expectedLast << ", actual " << last->second << "\n";
            return false;
        }
        return true;
    };
}

TEST(PubSub, BasicTest)
{
    using namespace tbd;
    PubSub pubsub{};
    auto func = [](int, const char *, long, long) {};
    const char *text = "abc";
    PubSub::Select foo{[](int, const char *, long, long) {}, 1, text};
    ASSERT_EQ(typeid(void), foo.ReturnType());
    ASSERT_EQ(typeid(std::tuple<const int &, const char *const &, long const &, long const &>), foo.ArgumentType());
    ASSERT_EQ(typeid(std::tuple<const int, const char *const, Any_t, Any_t>), foo.SelectArgs());

    std::vector<std::string> results{};
    auto sub1 = pubsub.Subscribe([&results](int v)
                                 { results.emplace_back("sub1:" + std::to_string(v)); }, 42);
    auto sub2 = pubsub.Subscribe(tbd::PubSub::Select{[&results](int v)
                                                      { results.emplace_back("sub2:" + std::to_string(v)); }, 42});
    auto sub3 = pubsub.Subscribe(Select([&results](int v)
                                        { results.emplace_back("sub3:" + std::to_string(v)); }, 42));

    auto sub4 = pubsub.Subscribe([&results](int a, int b)
                                 { results.emplace_back("sub4:" + std::to_string(a) + "," + std::to_string(b)); }, 42);
    auto sub5 = pubsub.Subscribe([&results](int a, int b)
                                 { results.emplace_back("sub5:" + std::to_string(a) + "," + std::to_string(b)); }, tbd::any, 69);

    pubsub.Publish(41);
    pubsub.Publish(42);
    pubsub.Publish(43);
    pubsub.Publish(42, 69);

    sub4 = nullptr;
    pubsub.Publish(42, 69);
    std::vector<std::string> expected{"sub1:42", "sub2:42", "sub3:42", "sub5:42,69", "sub4:42,69", "sub5:42,69"};
    ASSERT_EQ(expected , results);
}

TEST(PubSub, TextParameter)
{
    tbd::PubSub pubsub{};

    std::vector<std::string> results{};
    auto anchor = pubsub.Subscribe([&results](int a, const char *text)
                                   { results.emplace_back("1:" + std::to_string(a) + "," + text); }, 42);

    auto anchor2 = pubsub.Subscribe([&results](int a, const char *text)
                                    { results.emplace_back("2:" + std::to_string(a) + "," + text); }, tbd::any, std::string{"second"});

    pubsub.Publish(42, "first");

    pubsub.Publish(42, "second");

    std::vector<std::string> expected{"1:42,first", "2:42,second", "1:42,second"};
    ASSERT_EQ(expected, results);
}

TEST(PubSub, Recursion)
{
    tbd::PubSub pubsub{};

    tbd::PubSub::Anchor subAnchor{};
    ASSERT_FALSE(subAnchor);
    std::vector<std::string> results{};
    auto anchor = pubsub.Subscribe([&pubsub, &subAnchor, &results](int a)
                                   { if (!subAnchor) {
                                    subAnchor = pubsub.Subscribe([&subAnchor, &results](int b)
                                                                  {
                                                                        results.emplace_back("sub:" + std::to_string(b));
                                                                        subAnchor = nullptr; },
                                                                  69); } },
                                   42);
    ASSERT_TRUE(anchor);
    pubsub.Publish(69);
    ASSERT_TRUE(results.empty());
    pubsub.Publish(42);
    ASSERT_TRUE(results.empty());
    ASSERT_TRUE(subAnchor);
    pubsub.Publish(69);
    std::vector<std::string> expected{"sub:69"};
    ASSERT_EQ(expected, results);
    results.clear();
    pubsub.Publish(69);
    ASSERT_TRUE(results.empty());
    pubsub.Publish(42);
    ASSERT_TRUE(results.empty());
    pubsub.Publish(42);
    ASSERT_TRUE(results.empty());
    pubsub.Publish(69);
    ASSERT_EQ(expected, results);
    results.clear();
    pubsub.Publish(69);
    ASSERT_TRUE(results.empty());
}

TEST(PubSub, AnchorSync)
{
    std::latch started{2U};
    std::latch release{1U};
    std::latch release2{1U};
    tbd::PubSub pubsub{};
    std::promise<void> p{};
    auto f = p.get_future();
    auto anchor = pubsub.Subscribe(tbd::Select([&started, &release](int)
                                          {
        started.count_down();
        release.wait(); }, 42),
                                   tbd::Select([&started, &release2](int)
                                          {
        started.count_down();
        release2.wait(); }, 43));

    std::thread thr1{[&pubsub]{
        pubsub.Publish(42); // blocks on callback
    }};
    std::thread thr2{[&pubsub]{
        pubsub.Publish(43); // blocks on callback
    }};
    std::thread thr3{[&anchor, &started, &p]{
        started.wait();
        anchor = nullptr; // will wait because callback is active
        p.set_value();
    }};

    ASSERT_EQ(std::future_status::timeout, f.wait_for(shortDelay));
    release.count_down(); // callback may exit
    ASSERT_EQ(std::future_status::timeout, f.wait_for(shortDelay));
    release2.count_down(); // callback may exit
    ASSERT_EQ(std::future_status::ready, f.wait_for(shortDelay));

    f.get();

    thr1.join();
    thr2.join();
    thr3.join();
}

TEST(PubSub, Precision)
{
    unsigned int triggerValue{};
    unsigned int triggerCount{};

    tbd::PubSub pubsub{};
    std::deque<tbd::PubSub::Anchor> anchors{};

    for (unsigned int i = 0U; i < 50U; ++i)
    {
        anchors.emplace_back(pubsub.Subscribe([i,&triggerValue, &triggerCount] (unsigned int value) {
            if (i == value) {
                triggerValue = value;
                ++triggerCount;
            }
        }, i));
    }

    pubsub.Publish(42U);
    ASSERT_EQ(42U, triggerValue);
    ASSERT_EQ(1U, triggerCount);
}

TEST(PubSub, ComparisonModifiers)
{
    ASSERT_TRUE(show<tbd::GE<int>>({9, 99}, 11, 9, 11));
    ASSERT_TRUE(show<tbd::GT<int>>({9, 13}, 11, 9, 10));
    ASSERT_TRUE(show<tbd::LE<int>>({9, 13}, 11, 11, 13));
    ASSERT_TRUE(show<tbd::LT<int>>({9, 13}, 11, 12, 13));
}

TEST(PubSub, ExpireOnTime)
{
    // One anchor may reference multiple subscriptions, and if one of them is
    // tracking time, then we have an easy way of expiring an event after a
    // pre-determined time period, assuming we have a timer thread feeding
    // period time events.
    using namespace std::chrono_literals;
    tbd::PubSub pubsub{};
    int latest{};

    tbd::PubSub::Anchor anchor;
    auto now = std::chrono::steady_clock::now();

    anchor = pubsub.Subscribe(tbd::Select([&anchor](std::chrono::steady_clock::time_point)
                                          { anchor = nullptr; }, tbd::GE(now + 10s)),
                              tbd::Select([&latest](int v)
                                          { latest = v; }));
    pubsub.Publish(1);
    ASSERT_EQ(1, latest);
    pubsub.Publish(now);
    pubsub.Publish(2);
    ASSERT_EQ(2, latest);
    pubsub.Publish(now + 9s);
    pubsub.Publish(3);
    ASSERT_EQ(3, latest);
    pubsub.Publish(now + 10s);
    pubsub.Publish(4);
    ASSERT_EQ(3, latest);
}