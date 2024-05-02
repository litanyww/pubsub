#include "demangle.h"
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
                42)
            .Final();

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

TEST(PubSub, PerfNoSubscription)
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
TEST(PubSub, PerfOneSubscriptionNoMatch)
{
    tbd::PubSub pubsub;
    auto anchor = pubsub.Subscribe([](int) {}, 42);
    constexpr auto iterations = 1'000'000UL;
    using T = std::remove_const_t<decltype(iterations)>;

    Perf m{};
    while (m())
    {
        pubsub.Publish(69);
    }
    std::cerr << "one subscription no match perf: " << m << "\n";
}
TEST(PubSub, PerfOneSubscriptionMatch)
{
    tbd::PubSub pubsub;
    auto anchor = pubsub.Subscribe([](int) {}, 42);
    constexpr auto iterations = 1'000'000UL;
    using T = std::remove_const_t<decltype(iterations)>;

    Measure m{ iterations };
    for (T i{}; i < iterations; ++i)
    {
        pubsub.Publish(42);
    }
    m.Stop();
    std::cerr << "one subscription match perf: " << m << "\n";
}

TEST(PubSub, PerfOneKSubscriptionNoMatch)
{
    constexpr auto iterations = 1'000'000UL;
    constexpr auto subs = 1'000;
    tbd::PubSub pubsub;
    std::deque<tbd::PubSub::Anchor> anchors{};
    for (std::remove_const_t<decltype(subs)> i{}; i < subs; ++i)
    {
        anchors.push_back(pubsub.Subscribe([](int i) { std::cerr << "MATCH! " << i << "\n"; }, i));
    }

    Measure m{ iterations };
    for (std::remove_const_t<decltype(iterations)> i{}; i < iterations; ++i)
    {
        pubsub.Publish(static_cast<int>(1042));
    }
    m.Stop();
    std::cerr << "1k subscription no match perf: " << m << "\n";
}

TEST(PubSub, PerfOneKSubscriptionMatch)
{
    constexpr auto iterations = 1'000'000UL;
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

    Measure m{ iterations };
    for (std::remove_const_t<decltype(iterations)> i{}; i < iterations; ++i)
    {
        pubsub.Publish(static_cast<int>(i));
    }
    m.Stop();
    std::cerr << "1k subscription match perf: " << m << "\n";
}

enum class Op
{
    ProcessStart, // pid, path
    FileOpen,     // pid, fd, how, path
    FileClose,    // pid, fd
    ProcessEnd,   // pid
    FileDelete,   // pid, path
};

enum class Suspicious
{
    Mark, // Sus, path - Mark path as suspicious
    Start, // Sus, pid, path - Indicate that an executable started on a tainted executable
};

enum class How
{
    Read = 1,
    Write = 2,
    Exec = 4,
};
How& operator|=(How& lhs, How rhs)
{
    lhs =
        static_cast<How>(static_cast<std::underlying_type_t<How>>(lhs) | static_cast<std::underlying_type_t<How>>(rhs));
    return lhs;
}
How& operator&=(How& lhs, How rhs)
{
    lhs =
        static_cast<How>(static_cast<std::underlying_type_t<How>>(lhs) & static_cast<std::underlying_type_t<How>>(rhs));
    return lhs;
}
How& operator^=(How& lhs, How rhs)
{
    lhs =
        static_cast<How>(static_cast<std::underlying_type_t<How>>(lhs) ^ static_cast<std::underlying_type_t<How>>(rhs));
    return lhs;
}
How operator|(How lhs, How rhs)
{
    lhs |= rhs;
    return lhs;
}
How operator&(How lhs, How rhs)
{
    lhs &= rhs;
    return lhs;
}
How operator^(How lhs, How rhs)
{
    lhs ^= rhs;
    return lhs;
}

template<class Stream, typename = typename Stream::char_type>
Stream& operator<<(Stream& stream, How h)
{
    const char* comma = "";
    if ((h & How::Read) == How::Read)
    {
        stream << comma << "Read";
        comma = "|";
    }
    if ((h & How::Write) == How::Write)
    {
        stream << comma << "Write";
        comma = "|";
    }
    if ((h & How::Exec) == How::Exec)
    {
        stream << comma << "Exec";
        comma = "|";
    }
    return stream;
}

using pid_t = int;

void SimSub(
    const tbd::PubSub& pubsub,
    pid_t pid = 1234,
    const char* procName = "/procName",
    const char* fileName = "/fileName",
    std::function<void()> payload = nullptr)
{
    constexpr int fd{ 42 };
    pubsub(Op::ProcessStart, pid, procName);
    pubsub(Op::FileOpen, pid, fd, How::Write, fileName);
    pubsub(Op::FileClose, pid, fd);
    if (payload)
    {
        payload();
    }
    pubsub(Op::ProcessEnd, pid);
}

tbd::PubSub::Anchor processStarted(tbd::PubSub& pubsub, pid_t pid)
{
    auto anchor = pubsub.MakeAnchor();
    anchor.Add(
        [pubsub, anchors = pubsub.MakeAnchorage()](Op, pid_t pid, int fd, How how, const char* filePath) mutable
        {
            static_cast<void>(fd);
            // a file has been opened
            if ((how & How::Write) == How::Write)
            {
                auto anchor = pubsub.MakeAnchor();
                anchor.Add(
                    [pubsub, filePath, term = anchor.GetTerminator()](Op, pid_t, int)
                    {
                        pubsub(Suspicious::Mark, filePath);
                        term.Terminate();
                    },
                    Op::FileClose,
                    pid);
                anchor.Add([term = anchor.GetTerminator()](Op, pid_t) { term.Terminate(); }, Op::ProcessEnd, pid);
                anchors.push_back(std::move(anchor));
            }
        },
        Op::FileOpen,
        pid);
    anchor.Add(
        [term = anchor.GetTerminator()](Op, pid_t)
        {
            // if the process ends, we stop watching for file operations on this pid
            term.Terminate();
        },
        Op::ProcessEnd,
        pid);
    return anchor;
}

TEST(PubSub, Example)
{
    tbd::PubSub pubsub{};
    auto susRule = pubsub.MakeAnchor();

    susRule.Add(
        [pubsub, anchors = pubsub.MakeAnchorage()](Suspicious, const char* path) mutable
        {
            // std::cerr << "tainted executable marked: " << path << "\n";
            auto anchor = pubsub.Subscribe(
                [pubsub, anchors = pubsub.MakeAnchorage()](Op, pid_t pid, const char* path) mutable
                {
                    // std::cerr << "tainted executable started: " << path << "\n";
                    pubsub(Suspicious::Start, pid, path);
                    anchors.push_back(processStarted(pubsub, pid));
                },
                Op::ProcessStart,
                tbd::any,
                std::string{ path });
            anchor.Add(
                [pubsub, term = anchor.GetTerminator()](Op, pid_t, const char* path)
                {
                    static_cast<void>(path);
                    // std::cerr << "tainted executable deleted: " << path << "\n";
                    term.Terminate();
                },
                Op::FileDelete,
                tbd::any,
                std::string{ path });
            anchors.emplace_back(std::move(anchor));
        },
        Suspicious::Mark);

    unsigned int hitCount{};
    auto checker =
        pubsub.Subscribe([&hitCount](Suspicious, pid_t, const char* path) mutable { ++hitCount; }, Suspicious::Start);

    SimSub(pubsub, 1024, "/notTained", "/taintedFile");
    pubsub(Op::ProcessStart, static_cast<pid_t>(1025), "/taintedFile");
    ASSERT_EQ(0U, hitCount) << "nothing marked as suspicious yet";

    pubsub(Suspicious::Mark, "/maliciousFile");
    ASSERT_EQ(0U, hitCount) << "file not tainted yet";
    pubsub(Op::ProcessStart, static_cast<pid_t>(1026), "/taintedFile");
    ASSERT_EQ(0U, hitCount) << "file not tainted yet";

    SimSub(pubsub, 1027, "/maliciousFile", "/taintedFile"); // tainting
    ASSERT_EQ(1U, std::exchange(hitCount, 0U)) << "a tainted file was started, and tainted another";
    pubsub(Op::ProcessStart, static_cast<pid_t>(1028), "/taintedFile");
    ASSERT_EQ(1U, std::exchange(hitCount, 0U)) << "Executable now marked as tainted";

    pubsub(Op::ProcessStart, static_cast<pid_t>(1029), "/taintedFile");
    ASSERT_EQ(1U, std::exchange(hitCount, 0U)) << "Still tainted";
    pubsub(Op::FileDelete, static_cast<pid_t>(1030), "/taintedFile");
    pubsub(Op::ProcessStart, static_cast<pid_t>(1031), "/taintedFile");
    ASSERT_EQ(0U, hitCount) << "File no longer tainted";
}