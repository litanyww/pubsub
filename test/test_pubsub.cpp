#include "pubsub.h"

#include <gtest/gtest.h>
#include <typeindex>
#include <iostream>
#include <vector>
#include <string>


TEST(PubSub, BasicTest)
{
    using namespace tbd;
    PubSub pubsub{};
    auto func = [](int, const char *, long, long) {};
    const char *text = "abc";
    PubSub::Element foo{func, 1, text};
    ASSERT_EQ(typeid(void), foo.ReturnType());
    ASSERT_EQ(typeid(std::tuple<const int &, const char *const &, long const &, long const &>), foo.ArgumentType());
    ASSERT_EQ(typeid(std::tuple<int, const char *, Any_t, Any_t>), foo.SelectArgs());

    std::vector<std::string> results{};
    auto sub1 = pubsub.Subscribe([&results](int v)
                                 { results.emplace_back("sub1:" + std::to_string(v)); }, 42);
    auto sub2 = pubsub.Subscribe(tbd::PubSub::Element{[&results](int v)
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