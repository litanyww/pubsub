#include "pubsub.h"

#include <gtest/gtest.h>

#include <chrono>
#include <deque>
#include <future>
#include <iostream>
#include <latch>
#include <string>
#include <thread>
#include <typeindex>
#include <vector>

namespace
{
    using namespace std::chrono_literals;
    auto shortDelay = 10ms;

    struct TCompare
    {
        using is_transparent = void;

        template<typename L, typename R>
        bool operator()(const L& lhs, const R& rhs) const
        {
            bool result = lhs < rhs;
            // std::cerr << " " << lhs << " < " << rhs << " = " << std::boolalpha << result << "\n";
            return result;
        }
    };

    template<typename T>
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

    class Copied
    {
    public:
        struct Content
        {
            unsigned int c{};
            unsigned int m{};
        };

    private:
        Content* x_{};

    public:
        Copied() = default;
        Copied(Content& c) : x_{ &c } {}
        Copied(const Copied& copy) : x_{ copy.x_ } { ++x_->c; }
        Copied& operator=(const Copied& copy)
        {
            x_ = copy.x_;
            ++x_->c;
            return *this;
        }
        Copied(Copied&& donor) : x_{ std::exchange(donor.x_, nullptr) } { ++x_->m; }
        Copied& operator=(Copied&& donor)
        {
            std::swap(x_, donor.x_);
            ++x_->m;
            return *this;
        }
        template<typename Stream, typename = typename Stream::char_type>
        friend Stream& operator<<(Stream& stream, const Copied& v)
        {
            if (v.x_)
            {
                stream << "Copied[" << v.x_->c << "," << v.x_->m << "]";
            }
            else
            {
                stream << "Copied[empty]";
            }
            return stream;
        }
        unsigned int GetCopyCount() const { return x_ ? x_->c : 0U; }
        unsigned int GetMoveCount() const { return x_ ? x_->m : 0U; }
        // clang-format off
        friend auto operator<=>(const Copied& lhs, const Copied& rhs) { return lhs.x_ <=> rhs.x_; }
        // clang-format on
    };

} // namespace

TEST(PubSub, BasicTest)
{
    using namespace tbd;
    PubSub pubsub{};
    auto func = [](int, const char*, long, long) {};
    const char* text = "abc";
    PubSub::Select foo{ [](int, const char*, long, long) {}, 1, text };
    ASSERT_EQ(typeid(std::tuple<const int&, const char* const, long const&, long const&>), foo.ArgumentType())
        << Demangle(typeid(foo.ArgumentType()));
    ASSERT_EQ(typeid(std::tuple<const int, const char* const, Any_t, Any_t>), foo.SelectArgs());

    std::multiset<std::string> results{};
    auto sub1 = pubsub.Subscribe([&results](int v) { results.insert("sub1:" + std::to_string(v)); }, 42);
    auto sub2 = pubsub.Subscribe([&results](int v) { results.insert("sub2:" + std::to_string(v)); }, 42);
    auto sub3 = pubsub.Subscribe([&results](int v) { results.insert("sub3:" + std::to_string(v)); }, 42);

    auto sub4 = pubsub.Subscribe(
        [&results](int a, int b) { results.insert("sub4:" + std::to_string(a) + "," + std::to_string(b)); }, 42);
    auto sub5 = pubsub.Subscribe(
        [&results](int a, int b) { results.insert("sub5:" + std::to_string(a) + "," + std::to_string(b)); },
        tbd::any,
        69);

    pubsub.Publish(41);
    pubsub.Publish(42);
    pubsub.Publish(43);
    pubsub.Publish(42, 69);

    sub4 = nullptr;
    pubsub.Publish(42, 69);
    std::multiset<std::string> expected{ "sub1:42", "sub2:42", "sub3:42", "sub5:42,69", "sub4:42,69", "sub5:42,69" };
    ASSERT_EQ(expected, results);
}

TEST(PubSub, TextParameter)
{
    tbd::PubSub pubsub{};

    std::vector<std::string> results{};
    auto anchor =
        pubsub
            .Subscribe(
                [&results](int a, const char* text) { results.emplace_back("1:" + std::to_string(a) + "," + text); },
                42);

    auto anchor2 = pubsub.Subscribe(
        [&results](int a, const char* text) { results.emplace_back("2:" + std::to_string(a) + "," + text); },
        tbd::any,
        std::string{ "second" });

    pubsub.Publish(42, "first");

    pubsub.Publish(42, "second");

    std::vector<std::string> expected{ "1:42,first", "2:42,second", "1:42,second" };
    ASSERT_EQ(expected, results);
}

TEST(PubSub, Recursion)
{
    tbd::PubSub pubsub{};

    auto subAnchor = pubsub.MakeAnchor();
    ASSERT_FALSE(subAnchor);
    std::vector<std::string> results{};
    auto anchor = pubsub.MakeAnchor();
    anchor.Add(
        [&pubsub, &subAnchor, &results](int)
        {
            if (!subAnchor)
            {
                subAnchor.Add(
                    [term = subAnchor.GetTerminator(), &results](int b)
                    {
                        results.emplace_back("sub:" + std::to_string(b));
                        term.Terminate();
                    },
                    69);
            }
        },
        42);
    ASSERT_TRUE(anchor);
    pubsub.Publish(69);
    ASSERT_TRUE(results.empty());
    pubsub.Publish(42);
    ASSERT_TRUE(results.empty());
    ASSERT_TRUE(subAnchor);
    pubsub.Publish(69);
    std::vector<std::string> expected{ "sub:69" };
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
    std::latch started{ 2U };
    std::latch release{ 1U };
    std::latch release2{ 1U };
    tbd::PubSub pubsub{};
    std::promise<void> p{};
    auto f = p.get_future();
    auto anchor = pubsub.Subscribe(
        [&started, &release](int)
        {
            started.count_down();
            release.wait();
        },
        42);
    anchor.Add(
        [&started, &release2](int)
        {
            started.count_down();
            release2.wait();
        },
        43);

    std::thread thr1{ [&pubsub]
                      {
                          pubsub.Publish(42); // blocks on callback
                      } };
    std::thread thr2{ [&pubsub]
                      {
                          pubsub.Publish(43); // blocks on callback
                      } };
    std::thread thr3{ [&anchor, &started, &p]
                      {
                          started.wait();
                          anchor = nullptr; // will wait because callback is active
                          p.set_value();
                      } };

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
        anchors.emplace_back(pubsub.Subscribe(
            [i, &triggerValue, &triggerCount](unsigned int value)
            {
                if (i == value)
                {
                    triggerValue = value;
                    ++triggerCount;
                }
            },
            i));
    }

    pubsub.Publish(42U);
    ASSERT_EQ(42U, triggerValue);
    ASSERT_EQ(1U, triggerCount);
}

TEST(PubSub, ComparisonModifiers)
{
    ASSERT_TRUE(show<tbd::GE<int>>({ 9, 99 }, 11, 9, 11));
    ASSERT_TRUE(show<tbd::GT<int>>({ 9, 13 }, 11, 9, 10));
    ASSERT_TRUE(show<tbd::LE<int>>({ 9, 13 }, 11, 11, 13));
    ASSERT_TRUE(show<tbd::LT<int>>({ 9, 13 }, 11, 12, 13));
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

    anchor =
        pubsub.Subscribe([&anchor](std::chrono::steady_clock::time_point) { anchor = nullptr; }, tbd::GE(now + 10s))
            .Subscribe([&latest](int v) { latest = v; });
    pubsub(1);
    ASSERT_EQ(1, latest);
    pubsub.Publish(now);
    pubsub.Publish(2);
    ASSERT_EQ(2, latest);
    pubsub(now + 9s);
    pubsub(3);
    ASSERT_EQ(3, latest);
    pubsub(now + 10s);
    pubsub(4);
    ASSERT_EQ(3, latest);
}

int latestNotALambdaArgument{};
void NotALambda(int, int i)
{
    latestNotALambdaArgument = i;
}

TEST(PubSub, FunctionPointer)
{
    // we want this to work with function pointers, not just lambdas
    tbd::PubSub pubsub;

    auto anchor = pubsub.Subscribe(NotALambda, 42);
    pubsub.Publish(42, 123);
    ASSERT_EQ(123, latestNotALambdaArgument);
}

TEST(PubSub, CopyCount)
{
    tbd::PubSub pubsub;

    Copied::Content x{};

    auto anchor = pubsub.Subscribe([](const Copied&) {});
    pubsub(Copied{ x });
    ASSERT_EQ(0U, x.c);
    ASSERT_EQ(0U, x.m);

    anchor = pubsub.Subscribe([](const Copied&) {}, Copied{ x });
    ASSERT_EQ(0U, x.c);
    ASSERT_EQ(1U, x.m);
}
