#pragma once
// #include <functional>

#include <cstddef>
#include <deque>
#include <list>
#include <memory>
#include <set>
#include <tuple>
#include <typeindex>
#include <typeinfo>
#include <map>
#include <shared_mutex>
#include <mutex>
#include <thread>

//
#include <iostream>

namespace tbd
{
    class Any_t
    {
    public:
        friend constexpr auto operator<=>(const Any_t&, const Any_t&) = default;

        template <class Other>
        friend constexpr auto operator<=>(const Any_t &, const Other &other) { return other <=> other; }

        template <class Other>
        friend constexpr bool operator==(const Any_t &, const Other &other) { return true; }

        template <class Stream, class = Stream::char_type>
        friend Stream &operator<<(Stream &stream, const Any_t &)
        {
            stream << "any";
            return stream;
        }
    };
    constexpr static Any_t any;

    template <class... Args>
    using ArgsToTuple = std::tuple<const std::remove_const_t<std::decay_t<Args>> &...>;

    template <class NewType, class PA, class TA>
    constexpr auto Extend(TA&& b)
    {
        if constexpr (std::tuple_size<TA>() < std::tuple_size<PA>())
        {
            return Extend<NewType, PA>(std::tuple_cat(std::forward<TA>(b), std::tuple<NewType>{}));
        }
        else
        {
            return b;
        }
    }
  
    template <class NewType, class TP, class TA>
    using ExtendedTupleType = decltype(Extend<NewType, TA, TP>({}, {}));

    template <typename Signature>
    class MemberDecode;

    template <class Res, class Class, class... ArgTypes>
    class MemberDecode<Res (Class::*)(ArgTypes...) const>
    {
    public:
        using RetType = Res;
        using TupleType = ArgsToTuple<ArgTypes...>;
    };
    template <class Res, class Class, class... ArgTypes>
    class MemberDecode<Res (Class::*)(ArgTypes...)>
    {
    public:
        using RetType = Res;
        using TupleType = ArgsToTuple<ArgTypes...>;
    };
    template <class Lambda>
    using GetRet = typename MemberDecode<decltype(&Lambda::operator())>::RetType;

    template <class Lambda>
    using GetTuple = typename MemberDecode<decltype(&Lambda::operator())>::TupleType;


    template <size_t count, class NewType, class... Args>
    constexpr auto AddTypeFunc(Args... args)
    {
        if (sizeof...(Args) < count) {
            return AddTypeFunc<count, NewType, Args..., NewType>(args..., NewType{});
        }
        else
        {
            return std::tuple<Args...>{args...};
        }
    }

    template <class Lambda, class NewType, class... Args>
    struct AddType
    {
        using Type = decltype(AddTypeFunc<std::tuple_size<GetTuple<Lambda>>(), NewType, Args...>(std::declval<Args>()...));
    };

    template <class Lambda, class NewType, class... Args>
    constexpr auto ExtendWithType(Args&&... args)
    {
        constexpr auto count = std::tuple_size<GetTuple<Lambda>>();
        if constexpr (sizeof...(Args) < count)
        {
            return ExtendWithType<Lambda, NewType>(std::forward<Args>(args)..., NewType{});
        }
        else
        {
            return std::tuple<Args...>{std::forward<Args>(args)...};
        }
    }


    class PubSub
    {
    public:
        class Linker;
        class ElementBase
        {
            std::weak_ptr<Linker> linker_{};
        public:
            void SetLinker(std::weak_ptr<Linker> linker) { linker_ = linker;}
            std::weak_ptr<Linker> GetLinker() { return linker_; }
            virtual ~ElementBase(){};
            virtual void *GetFunc() = 0;
            virtual void Execute(const void* args) = 0;
            virtual bool LessThan(const ElementBase* candidate) const = 0;
            virtual bool LessThan(const void* candidate) const = 0;
            virtual bool GreaterThan(const void* candidate) const = 0;
            virtual std::unique_ptr<ElementBase> MakeUnique() = 0;

            virtual std::type_index ReturnType() const = 0;
            virtual std::type_index ArgumentType() const = 0;
            virtual std::type_index SelectArgs() const = 0;

            class Compare
            {
            public:
                using is_transparent = void;
                bool operator()(const std::unique_ptr<ElementBase>& lhs, const std::unique_ptr<ElementBase>& rhs) const { return lhs->LessThan(rhs.get()); }
                template <class... Args>
                bool operator()(const std::unique_ptr<ElementBase>& lhs, const std::tuple<Args...>& rhs) const { return lhs->LessThan(static_cast<const void*>(&rhs)); }
                template <class... Args>
                bool operator()(const std::tuple<Args...>& lhs, const std::unique_ptr<ElementBase>& rhs) const { return rhs->GreaterThan(static_cast<const void*>(&lhs)); }
            };
        };

        /// @brief Elements with the same SelectType share the same set
        using GroupSelector = std::multiset<std::unique_ptr<ElementBase>, ElementBase::Compare>;
        
        class Linker
        {
            std::deque<std::pair<GroupSelector*, GroupSelector::iterator>> entries_{};
            std::mutex activeLock_{};
            std::shared_mutex sharedLock_{};
            std::set<std::thread::id> active_{};
        public:
            void Remember(GroupSelector& selectors, GroupSelector::iterator it)
            {
                entries_.emplace_back(&selectors, it);
            }
            ~Linker() { Destroy(); }
            void Destroy()
            {
                auto entries = std::move(entries_);
                {
                    std::scoped_lock<std::mutex> activeGuard{activeLock_};
                    if (auto it = active_.find(std::this_thread::get_id()); it != active_.end())
                    {
                        active_.erase(it);
                        sharedLock_.unlock_shared();
                    }
                }
                std::scoped_lock<std::shared_mutex> guard{sharedLock_};

                for (auto[selectors, it] : entries)
                {
                    selectors->erase(it);
                }
            }
            bool Mark()
            {
                std::scoped_lock<std::mutex> activeGuard{activeLock_};
                std::pair<std::set<std::thread::id>::iterator, bool> p = active_.emplace(std::this_thread::get_id());
                if (!p.second)
                {
                    return false;
                }
                sharedLock_.lock_shared();
                return true;
            }

            void Unmark()
            {
                std::scoped_lock<std::mutex> activeGuard{activeLock_};
                if (active_.erase(std::this_thread::get_id()) > 0)
                {
                    sharedLock_.unlock_shared();
                }
            }

            class Guard
            {
                std::weak_ptr<Linker> linker_{};
                bool claimed_{};
            public:
                Guard(std::shared_ptr<Linker> linker) : linker_{linker}, claimed_{linker->Mark()} {}

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
            static Guard Protect(std::shared_ptr<Linker> linker) { return Guard{std::move(linker)}; }
        };

        class RunContext
        {
            ElementBase* element;
            std::weak_ptr<Linker> linker; // for protecting the thread


        };

        class Anchor
        {
            std::shared_ptr<Linker> linker_{};
        public:
            Anchor() = default;
            Anchor(std::shared_ptr<Linker> linker) : linker_{std::move(linker)} {}
            ~Anchor()
            {
                if (auto linker = std::move(linker_))
                {
                    linker->Destroy();
                }
            }
            Anchor &operator=(std::nullptr_t)
            {
                auto tmp = std::move(*this);
                return *this;
            }
            Anchor(Anchor&&) = default;
            Anchor& operator=(Anchor&&) = default;
            Anchor(const Anchor&) = delete;
            Anchor& operator=(const Anchor&) = delete;
            explicit operator bool() const { return static_cast<bool>(linker_); }
        };

        template <class Func, class SelectType>
        class Select : public ElementBase
        {
            using TupleType = GetTuple<Func>;
            static inline constexpr std::size_t CallArgCount = std::tuple_size<TupleType>();

            SelectType sel_; // the select type is a common size, the func is not.
            Func func_;

        public:
            bool LessThan(const void* candidate) const override
            {
                auto rhs = static_cast<const TupleType*>(candidate);
                return sel_ < *rhs;
            }
            bool GreaterThan(const void* candidate) const override
            {
                auto rhs = static_cast<const TupleType*>(candidate);
                return sel_ > *rhs;
            }
            bool LessThan(const ElementBase* candidate) const override
            {
                auto rhs = static_cast<const Select*>(candidate);
                return sel_ < rhs->sel_;
            }
            void *GetFunc() override { return static_cast<void *>(&func_); }

            void Execute(const void* args) override
            {
                auto& params = *static_cast<const TupleType*>(args);
                std::apply(func_, params);
            }
            std::unique_ptr<ElementBase> MakeUnique() override
            {
                auto result = std::make_unique<Select>(std::move(*this));
                return result;
            }
            std::type_index ReturnType() const override { return std::type_index{typeid(GetRet<Func>)}; }
            std::type_index ArgumentType() const override{ return std::type_index{typeid(TupleType)}; }
            std::type_index SelectArgs() const override{ return std::type_index{typeid(SelectType)}; }

            template <class Lambda, class... Args>
            explicit Select(Lambda&& func, Args&&... args) :
                sel_{
                    Extend<Any_t,
                    GetTuple<Lambda>,
                    std::tuple<std::decay_t<Args>...>
                    >({std::forward<Args>(args)...})},
                func_{std::move(func)}
            {
            }
        };
        template <class Lambda, class... Args>
        Select(Lambda f, Args&&... a) -> Select<Lambda,
        decltype(
            Extend<Any_t,
                GetTuple<Lambda>,
                std::tuple<const std::decay_t<Args>...>
            >({})
        )>; 

        /// @brief Each prototype checks all GroupSelectors, but we need to index them to insert quickly
        using PerPrototype = std::map<std::type_index, GroupSelector>;

        class Data
        {
            std::map<std::type_index, PerPrototype> database_{};
            std::shared_mutex lock_{};

            using ScopedLock = std::scoped_lock<std::shared_mutex>;

        public:
            ScopedLock GetLock() { return ScopedLock{lock_}; }
            void AddElement(ScopedLock&, std::shared_ptr<Linker>&) {}

            template <class Func, class... Args, class... Rem>
            void AddElement(ScopedLock& guard, std::shared_ptr<Linker>& linker, Select<Func, Args...>&& first, Rem... rem)
            {
                auto& perPrototype = database_[first.ArgumentType()];
                auto& selectorSet = perPrototype[first.SelectArgs()];
                auto base = first.MakeUnique();
                auto it = selectorSet.insert(std::move(base));
                linker->Remember(selectorSet, it);
                (*it)->SetLinker(linker);

                AddElement(guard, linker, std::move(rem)...);
            }

            template <class Type>
            std::deque<std::weak_ptr<ElementBase>> GetMatches(Type argTuple)
            {
                std::deque<std::weak_ptr<ElementBase>> winners{};
                std::shared_lock<std::shared_mutex> guard{lock_};
                if (auto perPrototypeIt = database_.find(std::type_index{typeid(decltype(argTuple))}); perPrototypeIt != database_.end())
                {
                    PerPrototype& perPrototype = perPrototypeIt->second;
                    for (auto& [type, selectors] : perPrototype)
                    {
                        auto [first, last] = selectors.equal_range(argTuple);
                        for (;first != last; ++first)
                        {
                            ElementBase* element = first->get();
                            auto weak = element->GetLinker();
                            if (auto linker = element->GetLinker().lock())
                            {
                                winners.push_back(std::shared_ptr<ElementBase>{linker, element});
                            }
                        }

                    }
                }
                return winners;
            }

        };
        std::shared_ptr<Data> data_{std::make_shared<Data>()};

        template<class... Args>
        void Publish(Args&&... args)
        {
            ArgsToTuple<Args...> argTuple{std::forward<Args>(args)...};

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

        template <class Func, class... Args>
        [[nodiscard]] Anchor Subscribe(Func func, Args&&... args)
        {
            auto linker = std::make_shared<Linker>();
            auto guard = data_->GetLock();

            data_->AddElement(guard, linker,
            Select{
                std::move(func),
                std::forward<Args>(args)...});

            return Anchor{std::move(linker)};
        }

        template <class Func, class... Args, class... Elems>
        [[nodiscard]] Anchor Subscribe(Select<Func, Args...>&& element, Elems&&... remainder)
        {
            auto linker = std::make_shared<Linker>();
            auto guard = data_->GetLock();

            data_->AddElement(guard, linker, std::move(element), std::forward<Elems>(remainder)...);

            return Anchor{std::move(linker)};
        }
    };

    template <class Lambda, class... Args>
    auto Select(Lambda &&func, Args &&...args)
    {
        return PubSub::Select<Lambda,
                              decltype(Extend<Any_t,
                                              GetTuple<Lambda>, std::tuple<const std::decay_t<Args>...>>({}))>{
            std::forward<Lambda>(func), std::forward<Args>(args)...};
    }

    template <class Type>
    class LE
    {
        Type value_{};
    public:
        explicit LE(Type value) : value_{std::move(value)} {}
        ~LE() = default;
        LE() = default;
        LE(LE&&) = default;
        LE& operator=(LE&&) = default;
        LE(const LE&) = default;
        LE& operator=(const LE&) = default;

        friend bool operator<(const LE& lhs, const LE& rhs) { return lhs.value_ < rhs.value_; }
        friend bool operator<(const LE& lhs, const Type& rhs) { return lhs.value_ < rhs; }
        friend bool operator<(const Type&, const LE&) { return false; }

        template <class Stream, class = Stream::char_type>
        friend Stream& operator<<(Stream& stream, const LE& g)
        {
            stream << "LE<" << typeid(Type).name() << ">{" << g.value_ << "}";
            return stream;
        }
    };

    template<class Type>
    LE(Type&&) -> LE<Type>;

    template <class Type>
    class LT
    {
        Type value_{};
    public:
        explicit LT(Type value) : value_{std::move(value)} {}
        ~LT() = default;
        LT() = default;
        LT(LT&&) = default;
        LT& operator=(LT&&) = default;
        LT(const LT&) = default;
        LT& operator=(const LT&) = default;

        friend bool operator<(const LT& lhs, const LT& rhs) { return lhs.value_ < rhs.value_; }
        friend bool operator<(const LT& lhs, const Type& rhs) { return lhs.value_ <= rhs; }
        friend bool operator<(const Type&, const LT&) { return false; }

        template <class Stream, class = Stream::char_type>
        friend Stream& operator<<(Stream& stream, const LT& g)
        {
            stream << "LT<" << typeid(Type).name() << ">{" << g.value_ << "}";
            return stream;
        }
    };

    template<class Type>
    LT(Type&&) -> LT<Type>;

    template <class Type>
    class GE
    {
        Type value_{};
    public:
        explicit GE(Type value) : value_{std::move(value)} {}
        GE() = default;
        ~GE() = default;
        GE(GE&&) = default;
        GE& operator=(GE&&) = default;
        GE(const GE&) = default;
        GE& operator=(const GE&) = default;

        friend bool operator<(const GE& lhs, const GE& rhs) { return lhs.value_ < rhs.value_; }
        friend bool operator<(const GE&, const Type&) { return false; }
        friend bool operator<(const Type& lhs, const GE& rhs) { return lhs < rhs.value_; }

        template <class Stream, class = Stream::char_type>
        friend Stream& operator<<(Stream& stream, const GE& g)
        {
            stream << "GE<" << typeid(Type).name() << ">{" << g.value_ << "}";
            return stream;
        }
    };
    template<class Type>
    GE(Type&&) -> GE<Type>;

    template <class Type>
    class GT
    {
        Type value_{};
    public:
        explicit GT(Type value) : value_{std::move(value)} {}
        GT() = default;
        ~GT() = default;
        GT(GT&&) = default;
        GT& operator=(GT&&) = default;
        GT(const GT&) = default;
        GT& operator=(const GT&) = default;

        friend bool operator<(const GT& lhs, const GT& rhs) { return lhs.value_ < rhs.value_; }
        friend bool operator<(const GT&, const Type&) { return false; }
        friend bool operator<(const Type& lhs, const GT& rhs) { return lhs <= rhs.value_; }
        template <class Stream, class = Stream::char_type>
        friend Stream& operator<<(Stream& stream, const GT& g)
        {
            stream << "GT<" << typeid(Type).name() << ">{" << g.value_ << "}";
            return stream;
        }
    };
    template<class Type>
    GT(Type&&) -> GT<Type>;

}