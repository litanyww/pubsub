Pubsub
======

Pubsub offers publish/subscribe functionality.

Simple Overview
---------------

A subscription specifies a callback which is called when a corresponding event is published.

    tbd::PubSub pubsub{};

    auto noCondition = pubsub.Subscribe([](int value){
        std::cout << "publish called with int value " << value << std::endl;
    });

    pubsub.Publish(1234); // parameter does not matter, it always triggers `noCondition`
    pubsub(1234); // an alias for `Publish()`

Sometimes, a subscription is only required should one or more of the published parameters have a specific value.  Each subscription can optionally specify conditions to apply to each parameter.  These conditions need not have the same type as the parameter; they just have to be comparable.  Any unspecified condition is assumed to be 'tbd::any'; which is a value which always matches.

    auto withConditions = pubsub.Subscribe([](int value, const char*){
        std::cout << "publish only called for 'banana', value=" << value << std::endl;
    }, tbd::any, std::string{"banana"});

    pubsub(42, "no match");
    pubsub(42, "banana"); // match!
    pubsub(42, std::string{"banana"}); // no match; the type must match

There are no restrictons on the number or types of parameters which can be published.  Any call to Publish will call all corresponding subscriptions.

    tbd::PubSub pubsub{};
    auto anchor = pubsub.Subscribe([](std::string_view text, std::shared_ptr<int> shared) {
        std::cout << "publish called with text '" << text << "' and a shared pointer containing int value '" << *shared << "'\n";
    });

    auto s = std::make_shared<int>(42);
    pubsub(std::string_view{"text"}, s);

The value returned by the `Subscribe()` method acts as an anchor, and must be retained by the caller until the subscription is no longer required.  Any number of subscriptions may be registered to the same anchor object.  This object may be moved, but not copied.

    auto multipleSubscriptions = pubsub.Subscribe([](int) {}, 42)
                                       .Subscribe([](const char*) {}, std::string{"text condition"});
    multipleSubscriptions.Add([](int, long) {}, tbd::any, 1234L);

All three subscriptions are associated with the same anchor, and they will remain associated.  Destroying this one anchor will always destroy all three subscriptions, along with any further subscriptions which might have been added to the anchor later.

PubSub is thread-safe, and `Publish()` or `Subscribe()` may be called concurrently from multiple threads.  A natural outcome is that each callback may find itself being called concurrently by multiple threads, so subscriptions must ensure that they take their own precautions to handle multi-threaded operations.

If a subscription callback is in progress when the associated anchor object is destroyed, the thread destroying the anchor will wait until all callbacks associated with that anchor have completed before the delete operation returns.  Additional published events will not call the subscriptions which are being deleted, but all in-progress callbacks must complete.

Any anchor may be destroyed within any callback thread.  However, since the anchor object can't be copied, a 'terminator' object may be created from the anchor that can be copied and it can be used to destroy that anchor instead.

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
    anchor.Add([term = anchor.GetTerminator()](std::chrono::steady_clock::time_point) {
            term.Terminate();
        }, tbd::GE{now + 60s});
    anchor.Add([](int) {
            std::cerr << "active" << std::endl;
        }, 42);

    pubsub(42); // matches
    pubsub(now + 50s);
    pubsub(42); // matches
    pubsub(now + 70s); // anchor is destroyed
    pubsub(42); // no match

Another modifier is `BitSelect`, which can be used to select specific bits in an event.

    auto anchor = pubsub.Subscribe([](Op, pid_t pid, int fd, int flags, const char* filename) {
        std::cerr << "only open operations\n";
    }, Op::FileOpen, tbd::any, tbd::any, tbd::BitSelect<O_WRONLY>{O_WRONLY});

    pubsub(Op::FileOpen, 123, 3, O_RDWR, "/tmp/filename"); // matches (assuming O_RDWR = O_RDONLY | O_WRONLY)

Complex Event Analysis
----------------------

Identifying interesting correlations within a stream of events is traditionally done by keeping an indexed pool of events within a sliding window.  When the last event is found, the pool is scanned to confirm whether the necessary pre-requisite events are also present.

However, that's not the only way to perform complex event analysis.  For any one complex event to be identified, you just need the sequence of prerequisite events.  You wait for the first event, after which you wait for the second, and so on until the last in the chain is encountered.  Use a subscription to find the first in the chain, and within the subscription callback set up a subscription for the next, and so on.  This approach has a number of advantages.

 - It is not necessary to store and index all events
 - There's no sliding window, so events are not constrained by time.
 - Setting up the chain of subscriptions is straightforward.
 - Events which might otherwise be found in the chain but where the previous events have not yet been encountered result results in no work.
