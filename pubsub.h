#pragma once

#include "demangle.h"

#include <cstddef>
#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <thread>
#include <tuple>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace tbd
{
    template<class Stream>
    concept Streamable = requires(Stream& s) {
        typename Stream::char_type;
        { s << "" } -> ::std::same_as<Stream&>;
    };

    /** @brief class which always matches anything */
    class Any_t
    {
    public:
        // clang-format off
        friend constexpr auto operator<=>(const Any_t&, const Any_t&) = default;

        template<typename Other>
        friend constexpr auto operator<=>(const Any_t&, const Other& other)
        {
            return other <=> other;
        }
        // clang-format on

        template<typename Other>
        friend constexpr bool operator==(const Any_t&, const Other& other)
        {
            return true;
        }

        template<Streamable Stream>
        friend Stream& operator<<(Stream& stream, const Any_t&)
        {
            stream << "any";
            return stream;
        }
    };
    constexpr static Any_t any;

    namespace helpers
    {
        template<class T>
        struct ArgToTuple
        {
            using Type = const ::std::remove_const_t<T>&;
        };
        template<class T>
        struct ArgToTuple<T*>
        {
            using Type = T*;
        };

        template<class T>
        struct ArgToTuple<T&>
        {
            using Type = T&;
        };

        template<typename Type>
        using ArgToTuple_t = const typename ArgToTuple<::std::decay_t<Type>>::Type;

        template<typename... Args>
        using ArgsToTuple = ::std::tuple<ArgToTuple_t<Args>...>;

        template<typename NewType, typename PA, typename... TA>
        constexpr auto Extend(TA&&... args)
        {
            if constexpr (sizeof...(TA) < ::std::tuple_size<PA>())
            {
                return Extend<NewType, PA>(::std::forward<TA>(args)..., NewType{});
            }
            else
            {
                return ::std::tuple<TA...>(::std::forward<TA>(args)...);
            }
        }

        template<typename NewType, typename PA, typename... Args>
        using ExtendType = decltype(Extend<NewType, PA>(::std::declval<Args>()...));

        template<typename Tup, typename... Args>
        constexpr Tup ExtendTuple(Args&&... args)
        {
            if constexpr (sizeof...(Args) < ::std::tuple_size<Tup>())
            {
                return ExtendTuple<Tup>(
                    ::std::forward<Args>(args)..., decltype(::std::get<sizeof...(Args)>(::std::declval<Tup>())){});
            }
            else
            {
                return Tup{ ::std::forward<Args>(args)... };
            }
        }
        template<typename Signature>
        class MemberDecode;

        template<typename Res, typename Class, typename... ArgTypes>
        class MemberDecode<Res (Class::*)(ArgTypes...) const>
        {
        public:
            using RetType = Res;
            using TupleType = ArgsToTuple<ArgTypes...>;
        };
        template<typename Res, typename Class, typename... ArgTypes>
        class MemberDecode<Res (Class::*)(ArgTypes...)>
        {
        public:
            using RetType = Res;
            using TupleType = ArgsToTuple<ArgTypes...>;
        };

        // template <typename Lambda> using GetRet = typename MemberDecode<decltype(&Lambda::operator())>::RetType;

        template<typename Lambda>
        struct GetTuple
        {
            using Type = typename MemberDecode<decltype(&Lambda::operator())>::TupleType;
        };

        template<typename... Args>
        struct GetTuple<void (*)(Args...)>
        {
            using Type = ArgsToTuple<Args...>;
        };

        template<typename Lambda>
        using GetTuple_t = GetTuple<Lambda>::Type;

        template<typename Lambda, typename... Args>
        using SelType = ExtendType<Any_t, GetTuple_t<Lambda>, const ::std::decay_t<Args>...>;
    } // namespace helpers

    class PubSub
    {
    public:
        class Linker;
        class Data;
        class ElementBase;

        class ElementBaseCompare
        {
        public:
            using is_transparent = void;
            bool operator()(const ::std::unique_ptr<ElementBase>& lhs, const ::std::unique_ptr<ElementBase>& rhs) const;
            template<typename... Args>
            bool operator()(const ::std::unique_ptr<ElementBase>& lhs, const ::std::tuple<Args...>& rhs) const;
            template<typename... Args>
            bool operator()(const ::std::tuple<Args...>& lhs, const ::std::unique_ptr<ElementBase>& rhs) const;
        };

        /// @brief Elements with the same SelectType share the same set
        using GroupSelector = ::std::multiset<::std::unique_ptr<ElementBase>, ElementBaseCompare>;
        using ActiveThreads_t = ::std::unordered_set<::std::thread::id>;
        using PerPrototype = ::std::unordered_map<::std::type_index, GroupSelector>;
        using Database_t = ::std::unordered_map<::std::type_index, PerPrototype>;

        class ElementBase
        {
            ::std::weak_ptr<Linker> linker_{};
            GroupSelector::iterator next_{};
            GroupSelector* selectors_{};

            friend class Linker;
            friend class Data;

        public:
            ::std::weak_ptr<Linker> GetLinker() { return linker_; }
            virtual ~ElementBase(){};
            virtual void* GetFunc() = 0;
            virtual void Execute(const void* args) = 0;
            virtual ::std::weak_ordering Compare(const ElementBase* candidate) const = 0;
            virtual ::std::weak_ordering Compare(const void* candidate) const = 0;
            virtual ::std::unique_ptr<ElementBase> MakeUnique() = 0;

            // virtual std::type_index ReturnType() const = 0;
            virtual ::std::type_index ArgumentType() const = 0;
            virtual ::std::type_index SelectArgs() const = 0;

        };

        class Linker
        {
            ::std::deque<::std::pair<GroupSelector*, GroupSelector::iterator>> entries_{};
            ::std::mutex activeLock_{};
            ::std::shared_mutex sharedLock_{};
            ActiveThreads_t active_{};
            ::std::thread::id activeSolo_{};
            ::std::weak_ptr<Data> data_{};
            ElementBase* mostRecent_{};

        public:
            Linker(::std::weak_ptr<Data> data) : data_{ ::std::move(data) } {}
            ~Linker() { Destroy(); }
            Linker(Linker&&) = delete;

            static void Remember(::std::shared_ptr<Linker> self, GroupSelector& selectors, GroupSelector::iterator it)
            {
                auto previous = ::std::exchange(self->mostRecent_, it->get());
                GroupSelector::iterator next{ previous ? ::std::exchange(previous->next_, it) : it};
                ElementBase& element = **it;
                element.next_ = next;
                element.linker_ = self;
                element.selectors_ = &selectors;
            }
            explicit operator bool() const { return mostRecent_; }
            size_t size() const { return entries_.size(); }
            ::std::weak_ptr<Data> GetData() { return data_; }
            void Destroy()
            {
                auto last = ::std::exchange(mostRecent_, nullptr);
                if (!last)
                {
                    return;
                }
                {
                    ::std::scoped_lock<::std::mutex> activeGuard{ activeLock_ };
                    const auto thisThread = ::std::this_thread::get_id();
                    if (active_.empty())
                    {
                        if (activeSolo_ == thisThread)
                        {
                            activeSolo_ = {};
                            sharedLock_.unlock_shared();
                        }
                    }
                    else
                    {
                        if (auto it = active_.find(thisThread); it != active_.end())
                        {
                            active_.erase(it);
                            sharedLock_.unlock_shared();
                        }
                    }
                }
                ::std::scoped_lock<::std::shared_mutex> guard{ sharedLock_ };
                if (auto data = data_.lock())
                {
                    data->ReleaseNodes(*last);
                }
            }
            bool Mark()
            {
                {
                    const auto thisThread = ::std::this_thread::get_id();
                    ::std::scoped_lock<::std::mutex> activeGuard{ activeLock_ };
                    if (active_.empty())
                    {
                        if (activeSolo_ == ::std::thread::id{})
                        {
                            activeSolo_ = thisThread;
                        }
                        else if (activeSolo_ == thisThread)
                        {
                            return false;
                        }
                        else
                        {
                            active_.emplace(activeSolo_);
                            active_.emplace(thisThread);
                            activeSolo_ = {};
                        }
                        sharedLock_.lock_shared();
                        return true;
                    }

                    auto p = active_.emplace(thisThread);
                    if (!p.second)
                    {
                        return false;
                    }
                }
                sharedLock_.lock_shared();
                return true;
            }

            void Unmark()
            {
                {
                    ::std::scoped_lock<::std::mutex> activeGuard{ activeLock_ };
                    if (active_.empty())
                    {
                        if (activeSolo_ != ::std::this_thread::get_id())
                        {
                            return;
                        }
                        activeSolo_ = {};
                    }
                    else if (active_.erase(::std::this_thread::get_id()) == 0)
                    {
                        return;
                    }
                }
                sharedLock_.unlock_shared();
            }

            class Guard
            {
                ::std::weak_ptr<Linker> linker_{};
                bool claimed_{};

            public:
                Guard(::std::shared_ptr<Linker> linker) : linker_{ linker }, claimed_{ linker->Mark() } {}

                ~Guard()
                {
                    if (claimed_)
                    {
                        if (auto linker = linker_.lock())
                        {
                            linker->Unmark();
                        }
                    }
                }
            };
            static Guard Protect(::std::shared_ptr<Linker> linker) { return Guard{ ::std::move(linker) }; }
        };

        template <class Type>
        class MatchResults
        {
            size_t size_{};
            union
            {
                Type short_[1];
                ::std::deque<Type> long_;
            };
            using Short = decltype(short_);
            using Long = decltype(long_);

            constexpr inline static size_t maxShort = sizeof(short_) / sizeof(short_[0]);

        public:
            MatchResults() {}
            MatchResults(MatchResults&& donor) noexcept : size_{ donor.size_ }
            {
                if (size_ <= maxShort)
                {
                    for (size_t i = 0 ; i < size_ ; ++i)
                    {
                        new (&short_[i]) Type {::std::move(donor.short_[i])};
                        donor.short_[i].~Type();
                    }
                }
                else
                {
                    new (&long_) Long{::std::move(donor.long_)};
                    donor.long_.~Long();
                }
            }
            ~MatchResults()
            {
                if (size_ <= maxShort)
                {
                    for (size_t i = 0 ; i < size_; ++i)
                    {
                        short_[i].~Type();
                    }
                }
                else
                {
                    long_.~Long();
                }
            }
            void push_back(Type t)
            {
                if (size_ < maxShort)
                {
                    new (&short_[size_++]) Type{ ::std::move(t) };
                }
                else if (size_ == maxShort)
                {
                    ++size_;
                    ::std::deque<Type> d{};
                    for (auto& x : short_)
                    {
                        d.push_back(::std::move(x));
                        x.~Type();
                    }
                    d.push_back(::std::move(t));
                    new (&long_) Long{ ::std::move(d) };
                }
                else {
                    ++size_;
                    long_.push_back(::std::move(t));
                }
            }
            class const_iterator
            {
                friend class MatchResults;
                using ItShort = const Type*;
                using ItLong = typename MatchResults::Long::const_iterator;
                using ItType = ::std::variant<ItShort, ItLong>;

                ItType iterator_{ItShort{}};

                const_iterator(ItType it) : iterator_{it} {}
            public:
                const_iterator() {}
                // clang-format off
                friend auto operator<=>(const const_iterator& lhs, const const_iterator& rhs) = default;
                // clang-format on
                const_iterator& operator++()
                {
                    ::std::visit([](auto&& it) { ++it; }, iterator_);
                    return *this;
                }
                const Type& operator*() const
                {
                    // return std::visit([](auto&& it) { return *it; }, iterator_);
                    if (::std::holds_alternative<ItShort>(iterator_))
                    {
                        return *::std::get<ItShort>(iterator_);
                    }
                    else
                    {
                        return *::std::get<ItLong>(iterator_);
                    }
                }
            };
            const_iterator begin() const {
                using ItType = const_iterator::ItType;
                return const_iterator{size_ <= maxShort ? ItType{::std::begin(short_)} : ItType{::std::begin(long_)}};
            }
            const_iterator end() const {
                using ItType = const_iterator::ItType;
                return const_iterator{size_ <= maxShort ? ItType{::std::begin(short_) + size_} : ItType{::std::end(long_)}};
            }
        };

        /** Simpler, non-movable variant of std::shared_lock<> */
        template <class Lock>
        class SharedGuard
        {
            Lock& lock_{};
        public:
            SharedGuard(Lock& lock) : lock_{lock} { lock.lock_shared(); }
            ~SharedGuard() { lock_.unlock_shared(); }
            SharedGuard(SharedGuard&&) = delete;
        };


        class Term
        {
            ::std::weak_ptr<Linker> linker_{};

        public:
            Term() = default;
            Term(::std::weak_ptr<Linker> linker) : linker_{ ::std::move(linker) } {}

            void Terminate() const
            {
                if (auto linker = linker_.lock())
                {
                    linker->Destroy();
                }
            }
        };

        class Anchor
        {
        protected:
            ::std::shared_ptr<Linker> linker_{};

        public:
            Anchor() = default;
            explicit Anchor(::std::shared_ptr<Linker> linker) : linker_{ ::std::move(linker) } {}
            virtual ~Anchor()
            {
                if (auto linker = ::std::move(linker_))
                {
                    linker->Destroy();
                }
            }
            Anchor& operator=(::std::nullptr_t)
            {
                auto tmp = ::std::move(*this);
                return *this;
            }
            Anchor(Anchor&&) = default;
            Anchor& operator=(Anchor&&) = default;
            Anchor(const Anchor&) = delete;
            Anchor& operator=(const Anchor&) = delete;
            explicit operator bool() const { return linker_ && *linker_; }
            size_t size() const { return linker_ ? linker_->size() : 0UL; }
            Term GetTerminator() const { return Term{ linker_ }; }

            template<typename Func, typename... Args>
            [[nodiscard]] Anchor Subscribe(Func func, Args&&... args)
            {
                Add(::std::move(func), ::std::forward<Args>(args)...);
                return Anchor{ ::std::move(linker_) };
            }

            template<typename Func, typename... Args>
            Anchor& Add(Func func, Args&&... args)
            {
                if (!linker_)
                {
                    throw ::std::runtime_error{ "Invalid anchor" };
                }

                if (auto data = linker_->GetData().lock())
                {
                    auto sel = ::std::make_unique<Select<Func, helpers::SelType<Func, Args...>>>(
                        ::std::move(func), ::std::forward<Args>(args)...);

                    data->AddElement(linker_, ::std::move(sel));
                }

                return *this;
            }
        };

        template<typename Func, typename SelectType>
        class Select : public ElementBase
        {
            using TupleType = helpers::GetTuple_t<Func>;
            static inline constexpr ::std::size_t CallArgCount = ::std::tuple_size<TupleType>();

            SelectType sel_; // the select type is a common size, the func is not.
            Func func_;

        public:
            ::std::weak_ordering Compare(const void* candidate) const override
            {
                auto rhs = static_cast<const TupleType*>(candidate);
                return sel_ <=> *rhs;
            }
            ::std::weak_ordering Compare(const ElementBase* candidate) const override
            {
                auto rhs = static_cast<const Select*>(candidate);
                return sel_ <=> rhs->sel_;
            }
            void* GetFunc() override { return static_cast<void*>(&func_); }

            void Execute(const void* args) override
            {
                auto& params = *static_cast<const TupleType*>(args);
                ::std::apply(func_, params);
            }
            ::std::unique_ptr<ElementBase> MakeUnique() override
            {
                auto result = ::std::make_unique<Select>(::std::move(*this));
                return result;
            }
            // std::type_index ReturnType() const override { return std::type_index{typeid(GetRet<Func>)}; }
            ::std::type_index ArgumentType() const override { return ::std::type_index{ typeid(TupleType) }; }
            ::std::type_index SelectArgs() const override { return ::std::type_index{ typeid(SelectType) }; }

            template<typename Lambda, typename... Args>
            explicit Select(Lambda&& func, Args&&... args) :
                sel_{ helpers::ExtendTuple<helpers::SelType<Lambda, Args...>>(::std::forward<Args>(args)...) },
                func_{ ::std::move(func) }
            {
            }
        };

        template<typename Lambda, typename... Args>
        Select(Lambda f, Args&&... a) -> Select<Lambda, helpers::SelType<Lambda, Args...>>;

        /// @brief Each prototype checks all GroupSelectors, but we need to index them to insert quickly

        /** Tag for PubSub constructor to force it to remove empty elements from
         * the subscription database
         * 
         * This is generally not recommended, because the number of empty
         * elements is set at compile time and is unlikely to be significant.
         */
        struct RemoveEmptySets{};

        class Data
        {
            Database_t database_{};
            ::std::shared_mutex lock_{};
            ::std::ostream* debugStream_{};
            bool removeEmptySets_{false};

            using ScopedLock = ::std::scoped_lock<::std::shared_mutex>;

        public:
            Data() {}
            explicit Data(::std::ostream& debugStream) : debugStream_{ &debugStream } {}
            explicit Data(PubSub::RemoveEmptySets) : removeEmptySets_{true} {}
            ScopedLock GetLock() { return ScopedLock{ lock_ }; }

            void AddElement(::std::shared_ptr<Linker>& linker, ::std::unique_ptr<ElementBase> base)
            {
                ScopedLock guard{ lock_ };
                auto argType = base->ArgumentType();
                auto& perPrototype = database_[base->ArgumentType()];
                auto& selectorSet = perPrototype[base->SelectArgs()];
                auto it = selectorSet.insert(::std::move(base));
                Linker::Remember(linker, selectorSet, it);
                if (debugStream_)
                {
                    *debugStream_ << "added : " << Demangle(argType) << "\n";
                }
            }

            template<typename Type>
            MatchResults<::std::weak_ptr<ElementBase>> GetMatches(Type argTuple)
            {
                MatchResults<::std::weak_ptr<ElementBase>> winners{};
                SharedGuard<::std::shared_mutex> guard{ lock_ };
                if (auto perPrototypeIt = database_.find(::std::type_index{ typeid(decltype(argTuple)) });
                    perPrototypeIt != database_.end())
                {
                    PerPrototype& perPrototype = perPrototypeIt->second;
                    for (auto& [type, selectors] : perPrototype)
                    {
                        auto [first, last] = selectors.equal_range(argTuple);
                        for (; first != last; ++first)
                        {
                            ElementBase* element = first->get();
                            if (auto linker = element->GetLinker().lock())
                            {
                                winners.push_back(::std::shared_ptr<ElementBase>{ linker, element });
                            }
                        }
                    }
                }
                else if (debugStream_)
                {
                    *debugStream_ << "no subscriptions for " << Demangle(typeid(Type)) << "\n";
                }
                return winners;
            }

            void ReleaseNodes(ElementBase& first)
            {
                bool removeEmpty{ false };
                ::std::set<::std::unique_ptr<ElementBase>> nodes{};
                {
                    ScopedLock guard{ lock_ };
                    auto it = first.next_;
                    for (;;)
                    {
                        auto& element = **it;
                        auto& selectors = element.selectors_;
                        auto next = element.next_;
                        nodes.insert(selectors->extract(it));
                        if (selectors->empty())
                        {
                            removeEmpty = true;
                        }
                        if (&element == &first)
                        {
                            break;
                        }
                        it = next;
                    }
                }
                nodes.clear();

                if (removeEmpty && removeEmptySets_)
                {
                    for (auto ppIt = database_.begin(); ppIt != database_.end();
                         ppIt->second.empty() ? (ppIt = database_.erase(ppIt)) : ++ppIt)
                    {
                        for (auto it = ppIt->second.begin(); it != ppIt->second.end();
                             it->second.empty() ? (it = ppIt->second.erase(it)) : ++it)
                        {
                        }
                    }
                }
            }
        };

        PubSub() = default;
        explicit PubSub(RemoveEmptySets arg) : data_{ ::std::make_shared<Data>(arg) } {}
        explicit PubSub(::std::ostream& debugStream) : data_{ ::std::make_shared<Data>(debugStream) } {}

        template<typename... Args>
        void Publish(Args&&... args) const
        {
            helpers::ArgsToTuple<Args...> argTuple{ args... };

            // unlock
            for (auto weak : data_->GetMatches(argTuple))
            {
                if (auto winner = weak.lock())
                {
                    if (auto linker = winner->GetLinker().lock())
                    {
                        auto guard = linker->Protect(linker);
                        winner->Execute(static_cast<const void*>(&argTuple));
                    }
                }
            }
        }

        template<typename... Args>
        void operator()(Args&&... args) const
        {
            Publish(::std::forward<Args>(args)...);
        }

        template<typename Func, typename... Args>
        [[nodiscard]] Anchor Subscribe(Func func, Args&&... args)
        {
            auto linker = ::std::make_shared<Linker>(data_);

            auto sel = ::std::make_unique<Select<Func, helpers::SelType<Func, Args...>>>(
                ::std::move(func), ::std::forward<Args>(args)...);

            data_->AddElement(linker, ::std::move(sel));

            return Anchor{ ::std::move(linker) };
        }

        [[nodiscard]] Anchor MakeAnchor() { return Anchor{ ::std::make_shared<Linker>(data_) }; }

        /** @brief Return a container in which to drop anchors
         * @return an empty container for anchors
         */
        static ::std::deque<Anchor> MakeAnchorage() { return ::std::deque<Anchor>{}; }

    private:
        ::std::shared_ptr<Data> data_{ ::std::make_shared<Data>() };
    };

    inline bool PubSub::ElementBaseCompare::operator()(
        const ::std::unique_ptr<ElementBase>& lhs,
        const ::std::unique_ptr<ElementBase>& rhs) const
    {
        return lhs->Compare(rhs.get()) == ::std::partial_ordering::less;
    }
    template<typename... Args>
    bool PubSub::ElementBaseCompare::operator()(
        const ::std::unique_ptr<ElementBase>& lhs,
        const ::std::tuple<Args...>& rhs) const
    {
        return lhs->Compare(static_cast<const void*>(&rhs)) == ::std::partial_ordering::less;
    }
    template<typename... Args>
    bool PubSub::ElementBaseCompare::operator()(
        const ::std::tuple<Args...>& lhs,
        const ::std::unique_ptr<ElementBase>& rhs) const
    {
        return rhs->Compare(static_cast<const void*>(&lhs)) == ::std::partial_ordering::greater;
    }

    constexpr PubSub::RemoveEmptySets removeEmptySets{};

    template<typename Type>
    class LE
    {
        Type value_{};

    public:
        explicit LE(Type value) : value_{ ::std::move(value) } {}
        ~LE() = default;
        LE() = default;
        LE(LE&&) = default;
        LE& operator=(LE&&) = default;
        LE(const LE&) = default;
        LE& operator=(const LE&) = default;

        friend bool operator<(const LE& lhs, const LE& rhs) { return lhs.value_ < rhs.value_; }
        friend bool operator<(const LE& lhs, const Type& rhs) { return lhs.value_ < rhs; }
        friend bool operator<(const Type&, const LE&) { return false; }

        template<Streamable Stream>
        friend Stream& operator<<(Stream& stream, const LE& g)
        {
            stream << "LE<" << typeid(Type).name() << ">{" << g.value_ << "}";
            return stream;
        }
    };

    template<typename Type>
    LE(Type&&) -> LE<Type>;

    template<typename Type>
    class LT
    {
        Type value_{};

    public:
        explicit LT(Type value) : value_{ ::std::move(value) } {}
        ~LT() = default;
        LT() = default;
        LT(LT&&) = default;
        LT& operator=(LT&&) = default;
        LT(const LT&) = default;
        LT& operator=(const LT&) = default;

        friend bool operator<(const LT& lhs, const LT& rhs) { return lhs.value_ < rhs.value_; }
        friend bool operator<(const LT& lhs, const Type& rhs) { return lhs.value_ <= rhs; }
        friend bool operator<(const Type&, const LT&) { return false; }

        template<Streamable Stream>
        friend Stream& operator<<(Stream& stream, const LT& g)
        {
            stream << "LT<" << typeid(Type).name() << ">{" << g.value_ << "}";
            return stream;
        }
    };

    template<typename Type>
    LT(Type&&) -> LT<Type>;

    template<typename Type>
    class GE
    {
        Type value_{};

    public:
        explicit GE(Type value) : value_{ ::std::move(value) } {}
        GE() = default;
        ~GE() = default;
        GE(GE&&) = default;
        GE& operator=(GE&&) = default;
        GE(const GE&) = default;
        GE& operator=(const GE&) = default;

        friend bool operator<(const GE& lhs, const GE& rhs) { return lhs.value_ < rhs.value_; }
        friend bool operator<(const GE&, const Type&) { return false; }
        friend bool operator<(const Type& lhs, const GE& rhs) { return lhs < rhs.value_; }

        template<Streamable Stream>
        friend Stream& operator<<(Stream& stream, const GE& g)
        {
            stream << "GE<" << typeid(Type).name() << ">{" << g.value_ << "}";
            return stream;
        }
    };
    template<typename Type>
    GE(Type&&) -> GE<Type>;

    template<typename Type>
    class GT
    {
        Type value_{};

    public:
        explicit GT(Type value) : value_{ ::std::move(value) } {}
        GT() = default;
        ~GT() = default;
        GT(GT&&) = default;
        GT& operator=(GT&&) = default;
        GT(const GT&) = default;
        GT& operator=(const GT&) = default;

        friend bool operator<(const GT& lhs, const GT& rhs) { return lhs.value_ < rhs.value_; }
        friend bool operator<(const GT&, const Type&) { return false; }
        friend bool operator<(const Type& lhs, const GT& rhs) { return lhs <= rhs.value_; }
        template<Streamable Stream>
        friend Stream& operator<<(Stream& stream, const GT& g)
        {
            stream << "GT<" << typeid(Type).name() << ">{" << g.value_ << "}";
            return stream;
        }
    };
    template<typename Type>
    GT(Type&&) -> GT<Type>;

    template <class Type, Type mask>
    class BitSelect
    {
        Type bits_{};
        friend auto operator<=>(const BitSelect& lhs, const BitSelect& rhs) = default;
        friend auto operator<=>(const BitSelect& lhs, const Type& rhs) {
            return lhs.bits_ <=> (rhs & mask);
        }
        friend bool operator==(const BitSelect& lhs, const Type& rhs) {
            return lhs.bits_ == (rhs & mask);
        }

        class AsOct
        {
            Type v;

            template<Streamable Stream>
            friend Stream& operator<<(Stream& stream, const AsOct& o)
            {
                auto flags = stream.flags(::std::ios::oct | (stream.flags() & (~::std::ios::basefield)));
                stream << "0" << o.v;
                stream.flags(flags);
                return stream;
            }

        public:
            AsOct(Type value) : v{ value } {}
        };

        template<class Stream>
            requires requires(Stream& s) {
                typename Stream::char_type;
                { s << "" } -> ::std::same_as<Stream&>;
            }
        friend Stream& operator<<(Stream& stream, const BitSelect& b)
        {
            if (b.bits_ == mask)
            {
                stream << "<" << AsOct(b.bits_) << ">";
            }
            else
            {
                stream << "<" << AsOct(mask) << "|" << AsOct(b.bits_) << ">";
            }
            return stream;
        }

    public:
        BitSelect() = default;
        explicit BitSelect(Type bits) : bits_{bits & mask} {}
        operator Type () const { return bits_; }
    };
} // namespace tbd