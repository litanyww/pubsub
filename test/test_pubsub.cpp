#include "pubsub.h"

#include <gtest/gtest.h>
#include <typeindex>
#include <iostream>


TEST(PubSub, BasicTest)
{
    using namespace tbd;
    PubSub pubsub{};
    auto func = [](int, const char*, long, long) {};
    const char* text = "abc";
    PubSub::Element foo{func, 1, text};
    ASSERT_EQ(typeid(void), foo.ReturnType());
    ASSERT_EQ(typeid(std::tuple<const int &, const char *const &, long const &, long const &>), foo.ArgumentType());
    ASSERT_EQ(typeid(std::tuple<int, const char*, Any_t, Any_t>), foo.SelectArgs());

    auto sub1 = pubsub.Subscribe([](int v){std::cerr << "sub1=" << v << "\n";}, 42);
    auto sub2 = pubsub.Subscribe(tbd::PubSub::Element{[](int v){std::cerr << "sub2=" << v << "\n";}, 42});
    auto sub3 = pubsub.Subscribe(Choose([](int v){std::cerr << "sub3=" << v << "\n";}, 42));

    auto sub4 = pubsub.Subscribe([](int v, int a2) {std::cerr << "sub4=" << v << "," << a2 << "\n";}, 42);
    auto sub5 = pubsub.Subscribe([](int v, int a2) {std::cerr << "sub5=" << v << "," << a2 << "\n";}, tbd::any, 69);

    pubsub.Publish(41);
    pubsub.Publish(42);
    pubsub.Publish(43);
    pubsub.Publish(42, 69);

    sub4 = nullptr;
    pubsub.Publish(42, 69);
}

TEST(PubSub, TextParameter)
{
    tbd::PubSub pubsub{};

    auto anchor = pubsub.Subscribe([](int, const char* text) {
        std::cerr << "text=" << text << "\n";
    }, 42);

    auto anchor2 = pubsub.Subscribe([](int, const char* text) {
        std::cerr << "matching with text selector: " << text << "\n"; }, tbd::any, std::string{"second"});

    pubsub.Publish(42, "first");

    pubsub.Publish(42, "second");
}