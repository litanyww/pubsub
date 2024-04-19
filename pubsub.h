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
        friend auto operator<=>(const Any_t&, const Any_t&) = default;
        template <class Other>
        friend auto operator<=>(const Any_t &, const Other &other) { return other <=> other; }
        template <class Other>
        friend bool operator==(const Any_t &, const Other &other) { return true; }
    };
    constexpr static Any_t any;

    template <unsigned int count, class AddType, class... Args>
    constexpr auto ExtendWithType(Args&&... args)
    {
        if constexpr (sizeof...(Args) < count)
        {
            return ExtendWithType<count, AddType>(std::forward<Args>(args)..., AddType{});
        }
        else
        {
            return std::tuple<Args...>{std::forward<Args>(args)...};
        }
    }

    template <std::size_t count, class NewType, class... Args>
    auto AddTypeFunc(Args... args)
    {
        if constexpr(sizeof...(Args) < count) {
            return AddTypeFunc<count, NewType, Args..., NewType>(args..., NewType{});
        }
        else
        {
            return std::tuple<Args...>{args...};
        }
    }

    template <std::size_t count, class NewType, class... Args>
    struct AddType
    {
        using Type = decltype(AddTypeFunc<count, NewType, Args...>(std::declval<Args>()...));
    };

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
            ~Linker()
            {
                {
                    std::scoped_lock<std::mutex> activeGuard{activeLock_};
                    if (auto it = active_.find(std::this_thread::get_id()); it != active_.end())
                    {
                        active_.erase(it);
                        sharedLock_.unlock_shared();
                    }
                }
                std::scoped_lock<std::shared_mutex> guard{sharedLock_};

                for (auto[selector, it] : entries_)
                {
                    selector->erase(it);
                }
            }
            std::pair<std::set<std::thread::id>::iterator, bool> Mark()
            {
                std::scoped_lock<std::mutex> activeGuard{activeLock_};
                std::pair<std::set<std::thread::id>::iterator, bool> p = active_.emplace(std::this_thread::get_id());
                if (p.second) {
                    sharedLock_.lock_shared();
                }
                return p;
            }

            void Unmark(std::set<std::thread::id>::iterator it)
            {
                std::scoped_lock<std::mutex> activeGuard{activeLock_};
                sharedLock_.unlock_shared();
                active_.erase(it);
            }

            class Guard
            {
                Linker& linker_;
                std::pair<std::set<std::thread::id>::iterator, bool> p_;
            public:
                Guard(Linker &linker) : linker_{linker}, p_{linker_.Mark()} {}

                ~Guard()
                {
                    if (p_.second)
                    {
                        linker_.Unmark(p_.first);
                    }
                }
            };
            Guard Protect() { return Guard{*this}; }
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
            Anchor(std::shared_ptr<Linker> linker) : linker_{std::move(linker)} {}
            ~Anchor() = default;
            Anchor& operator=(std::nullptr_t) { linker_ = nullptr; return *this; }
            Anchor(Anchor&&) = default;
            Anchor& operator=(Anchor&&) = default;
            Anchor(const Anchor&) = delete;
            Anchor& operator=(const Anchor&) = delete;
        };

        template <typename Signature>
        class MemberDecode;

        template <class... Args>
        using ArgsToTuple = std::tuple<const std::remove_const_t<std::decay_t<Args>> &...>;

        template <class Res, class Class, class... ArgTypes>
        class MemberDecode<Res (Class::*)(ArgTypes...) const>
        {
        public:
            using RetType = Res;
            using TupleType = ArgsToTuple<ArgTypes...>;
        };
        template <class Lambda>
        using GetRet = typename MemberDecode<decltype(&Lambda::operator())>::RetType;

        template <class Lambda>
        using GetTuple = typename MemberDecode<decltype(&Lambda::operator())>::TupleType;

        template <class Func, class... Args>
        class Element : public ElementBase
        {
            using RetType = GetRet<Func>;
            using TupleType = GetTuple<Func>;
            static inline constexpr std::size_t CallArgCount = std::tuple_size<TupleType>();
            using SelectType = AddType<CallArgCount, Any_t, Args...>::Type;

            Func func_;
            SelectType sel_;

        public:
            explicit Element(Func func, Args... args) : func_{std::move(func)},
                                                        sel_{ExtendWithType<CallArgCount, Any_t, Args...>(std::move(args)...)}
            {
            }
            bool LessThan(const void* candidate) const override
            {
                auto rhs = reinterpret_cast<const TupleType*>(candidate);
                return sel_ < *rhs;
            }
            bool GreaterThan(const void* candidate) const override
            {
                auto rhs = reinterpret_cast<const TupleType*>(candidate);
                return sel_ > *rhs;
            }
            bool LessThan(const ElementBase* candidate) const override
            {
                auto rhs = reinterpret_cast<const Element*>(candidate);
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
                return std::make_unique<Element>(std::move(*this));
            }
            std::type_index ReturnType() const override { return std::type_index{typeid(RetType)}; }
            std::type_index ArgumentType() const override{ return std::type_index{typeid(TupleType)}; }
            std::type_index SelectArgs() const override{ return std::type_index{typeid(SelectType)}; }
        };

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
            void AddElement(ScopedLock& guard, std::shared_ptr<Linker>& linker, Element<Func, Args...>&& first, Rem... rem)
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
                    auto linker = winner->GetLinker().lock();
                    auto guard = linker->Protect();
                    winner->Execute(static_cast<const void*>(&argTuple));
                }
            }
        }

        template <class Func, class... Args>
        [[nodiscard]] Anchor Subscribe(Func func, Args&&... args)
        {
            auto linker = std::make_shared<Linker>();
            auto guard = data_->GetLock();

            data_->AddElement(guard, linker, Element<Func, Args...>{std::move(func), std::forward<Args>(args)...});

            return Anchor{std::move(linker)};
        }

        template <class Func, class... Args, class... Elems>
        [[nodiscard]] Anchor Subscribe(Element<Func, Args...> first, Elems... elements)
        {
            auto linker = std::make_shared<Linker>();
            auto guard = data_->GetLock();

            data_->AddElement(guard, linker, std::move(first), std::move(elements)...);

            return Anchor{std::move(linker)};
        }
    };
    template <class Func, class... Args>
    PubSub::Element<Func, Args...> Select(Func&& func, Args&&... args) { return PubSub::Element<Func, Args...>{std::forward<Func>(func), std::forward<Args>(args)...}; }
}