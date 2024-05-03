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
    template<class Rep, class Period>
    constexpr size_t OperationsPerSecond(size_t iterations, std::chrono::duration<Rep, Period> elapsed)
    {
        return iterations * Period::den / (elapsed.count() * Period::num /* * Rep */);
    }

    class Measure
    {
        size_t iterations{};
        std::chrono::high_resolution_clock::time_point start{ std::chrono::high_resolution_clock::now() };
        std::chrono::high_resolution_clock::time_point end{};

    public:
        explicit Measure(size_t i) : iterations{ i } {}
        void Stop() { end = std::chrono::high_resolution_clock::now(); }
        template<typename Stream, typename = typename Stream::char_type>
        friend Stream& operator<<(Stream& stream, const Measure& m)
        {
            auto end = m.end.time_since_epoch().count() ? m.end : std::chrono::high_resolution_clock::now();
            auto ops = OperationsPerSecond(m.iterations, end - m.start);
            if (ops >= 1'000'000)
            {
                stream << ops / 1'000'000.0 << " mops";
            }
            else if (ops >= 1'000)
            {
                stream << ops / 1'000.0 << " kops";
            }
            else
            {
                stream << ops << " ops";
            }
            return stream;
        }
    };

    class Perf
    {
        size_t iterations{};
        std::chrono::high_resolution_clock::time_point start{ std::chrono::high_resolution_clock::now() };
        std::chrono::high_resolution_clock::time_point end{};

    public:
        Perf() : end{ start + std::chrono::milliseconds{ 50 } } {}

        template<class Rep, class Period>
        explicit Perf(std::chrono::duration<Rep, Period> duration) : end{ start + duration }
        {
        }

        bool operator()()
        {
            ++iterations;
            auto now = std::chrono::high_resolution_clock::now();
            if (now > end)
            {
                end = now;
                return false;
            }
            return true;
        }

        template<typename Stream, typename = typename Stream::char_type>
        friend Stream& operator<<(Stream& stream, const Perf& m)
        {
            auto ops = OperationsPerSecond(m.iterations, m.end - m.start);
            if (ops >= 1'000'000)
            {
                stream << ops / 1'000'000.0 << " mops";
            }
            else if (ops >= 1'000)
            {
                stream << ops / 1'000.0 << " kops";
            }
            else
            {
                stream << ops << " ops";
            }
            return stream;
        }
    };

}
TEST(Perf, NoSubscription)
{
    tbd::PubSub pubsub;
    constexpr auto iterations = 1'000'000UL;
    using T = std::remove_const_t<decltype(iterations)>;

    Perf m{};
    unsigned int i = 0;
    while (m())
    {
        pubsub.Publish(++i);
    }
    std::cerr << "no subscription perf: " << m << "\n";
}
TEST(Perf, OneSubscriptionNoMatch)
{
    tbd::PubSub pubsub;
    auto anchor = pubsub.Subscribe([](int) {}, 42);

    Perf m{};
    while (m())
    {
        pubsub.Publish(69);
    }
    std::cerr << "one subscription no match perf: " << m << "\n";
}
TEST(Perf, OneSubscriptionMatch)
{
    tbd::PubSub pubsub;
    auto anchor = pubsub.Subscribe([](int) {}, 42);

    Perf m{  };
    while(m())
    {
        pubsub.Publish(42);
    }
    std::cerr << "one subscription match perf: " << m << "\n";
}

TEST(Perf, OneKSubscriptionNoMatch)
{
    constexpr auto subs = 1'000;
    tbd::PubSub pubsub;
    std::deque<tbd::PubSub::Anchor> anchors{};
    for (std::remove_const_t<decltype(subs)> i{}; i < subs; ++i)
    {
        anchors.push_back(pubsub.Subscribe([](int i) { std::cerr << "MATCH! " << i << "\n"; }, i));
    }

    Perf m{ };
    while (m())
    {
        pubsub.Publish(static_cast<int>(1042));
    }
    std::cerr << "1k subscription no match perf: " << m << "\n";
}

TEST(Perf, OneKSubscriptionMatch)
{
    constexpr auto subs = 1'000;
    tbd::PubSub pubsub;
    std::deque<tbd::PubSub::Anchor> anchors{};
    Measure s(subs);
    for (std::remove_const_t<decltype(subs)> i{}; i < subs; ++i)
    {
        anchors.push_back(pubsub.Subscribe([](int) {}, i));
    }
    s.Stop();
    std::cerr << "1k subscription rate: " << s << "\n";

    Perf m{ };
    int i{};
    while (m())
    {
        pubsub.Publish(++i);
    }
    std::cerr << "1k subscription match perf: " << m << "\n";
}