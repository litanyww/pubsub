Pubsub
======

Pubsub offers publish/subscribe functionality.

A subscription specifies a callback which is called when a corresponding event is published.

    tbd::PubSub pubsub{};

    auto noCondition = pubsub.Subscribe([](int value){
        std::cout << "publish called with int value " << value << std::endl;
    });

    pubsub.Publish(1234); // parameter does not matter, it always triggers `noCondition`
    pubsub(1234); // this generates the same event.

A publish operation is a function call with any number of, or types, of parameters - in the above example, only a single integer parameter is provided, but any number of parameters of any type can be provided.  The subscribe call matches when it has the same prototype, and if the additonal conditions match.

    auto withConditions = pubsub.Subscribe([](int, const char* text){
        std::cout << "publish only called for int=42, and any text: " << text << std::endl;
    }, 42);

    pubsub(41, "no match");
    pubsub(42, "text passed in addition to matching condition");

Any value can be used as a condition as long as it can be compared with the corresponding event parameter.  If a condition is not provided, then it will always match.  In order to have a conditional match on a parameter where an earlier parameter is not considered, use the value of `any`.

The value returned by the `Subscribe()` method acts as an anchor, and must be retained by the caller until the subscription is no longer required.  Any number of subscriptions may be registered to the same anchor object.  This object may be moved, but not copied.

    auto multipleSubscriptions = pubsub.Subscribe([](int) {}, 42)
                                       .Subscribe([](const char*) {}, std::string{"text condition"});
    multipleSubscriptions.Add([](int, long) {}, tbd::any, 1234L);

All three subscriptions are associated with the same anchor, and they will remain associated.  Destroying this one anchor will always destroy all three subscriptions, and any further subscriptions which might be added to the anchor later.

PubSub is thread-safe, in that any thread is free to register subscriptions or publish events.  This requires some care within each callback, because callbacks might be called my multiple threads simultaneously.  If an anchor object is destroyed by one thread while a different thread called the associated event and that callback is still in progress, then the anchor object's destructor will be delayed until the callback is no longer active.

Any anchor may be destroyed within any callback thread.  However, bearing in mind that an anchor can't be copied, a terminator object can be created from the anchor which can be copied, and which may be used to terminate it safely.

    PubSub::Anchor MakeAnchor(tbd::PubSub pubsub)
    {
        auto anchor = pubsub.MakeAnchor();
        anchor.Add([term = anchor.GetTerminator()](int) { term.Terminate(); }, 42);
        return anchor;
    }

    auto anchor = MakeAnchor(pubsub);
    pubsub(42); // fires, destroying anchor
    pubsub(42); // does not fire, 
    anchor = nullptr; // would destroy the anchor if it was not already destroyed

The condition parameter need not be the same type as the event type, as seen above where a `std::string` is used as a condition to match a `const char*` event parameter.  Modifiers can be used to restrict matches:

    auto now = std::chrono::steady_clock::now();
    auto anchor = pubsub.MakeAnchor();
    anchor.Add([term = anchor.GetTerminator()](std::chrono::steady_clock::time_point) {term.Terminate(); }, tbd::GE{now + 60s});
    anchor.Add([](int) {
        std::cerr << "active" << std::endl;
    }, 42);

    pubsub(42); // matches
    pubsub(now + 70s); // anchor is destroyed
    pubsub(42); // no match