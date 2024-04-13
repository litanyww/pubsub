Pubsub
======

Pubsub offers publish/subscribe functionality.

A publish operation is a function call with any number of, or types of parameters.

A subscription provides a callback and an optional set of conditions.  When an event is published which has a matching set of parameters, its callback will be called.  The call which registers a subscription returns an object; this must be retained by the caller - once this anchor object is destroyed, the callback will no longer be called even when the parameters match.

    using namespace tbd;
    PubSub  pubsub{};

    PubSub::Anchor noCondition = pubsub.Subscribe([](int){
        // any event with a single integer parameter
    });

    PubSub::Anchor oneCondition = pubsub.Subscribe([](int, const char* text){
        // first parameter must be '42' to fire
    }, 42);

    pubsub.Publish(42);

    auto textAnchor = pubsub.Subscribe([](int, const char* text){
        // second parameter must compare equal with 'MatchThis'
    }, any, std::string{"MatchThis"});

    pubsub.Publish(69, "MatchThis"); // yep, that'll match
