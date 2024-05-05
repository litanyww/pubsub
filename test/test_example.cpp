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

void SimSubRead(
    const tbd::PubSub& pubsub,
    pid_t pid = 1234,
    const char* procName = "/procName",
    const char* fileName = "/fileName",
    std::function<void()> payload = nullptr)
{
    constexpr int fd{ 42 };
    pubsub(Op::ProcessStart, pid, procName);
    pubsub(Op::FileOpen, pid, fd, How::Read, fileName);
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
        },
        Op::FileOpen,
        pid,
        tbd::any,
        tbd::BitSelect(How::Write));
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

TEST(EventAnalysis, Example)
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
        pubsub.Subscribe([&hitCount](Suspicious, pid_t, const char*) mutable { ++hitCount; }, Suspicious::Start);

    SimSub(pubsub, 1024, "/notTained", "/taintedFile");
    pubsub(Op::ProcessStart, static_cast<pid_t>(1025), "/taintedFile");
    ASSERT_EQ(0U, hitCount) << "nothing marked as suspicious yet";

    pubsub(Suspicious::Mark, "/maliciousFile");
    ASSERT_EQ(0U, hitCount) << "file not tainted yet";
    pubsub(Op::ProcessStart, static_cast<pid_t>(1026), "/taintedFile");
    ASSERT_EQ(0U, hitCount) << "file not tainted yet";


    SimSubRead(pubsub, 1027, "/maliciousFile", "/taintedFile"); // read, so not tainted
    ASSERT_EQ(1U, std::exchange(hitCount, 0U)) << "a tainted file was started, but tainted nothing";
    pubsub(Op::ProcessStart, static_cast<pid_t>(1028), "/taintedFile");
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