#pragma once

#include <cstdint>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

namespace tbd
{
    // Simple pool.
    template<size_t Size>
    class Pools;

    using MaxOffsetType = unsigned int;

    template<size_t Size>
    class Pool
    {
        struct Element
        {
            Element* m_next;
        };
        struct Header
        {
            Element* m_firstFree{};
            Pools<Size>* m_pools{};
            unsigned int m_useCount{};
        };
        using OffsetType = std::conditional_t<(Size - sizeof(Element) < 0x10000), unsigned short, unsigned int>;
        struct Prefix
        {
            OffsetType m_offset;
        };

        Header m_header;
        std::byte m_data[Size - sizeof(Header)];
        static_assert(sizeof(m_data) > 0UL);

        static std::byte* Align(std::byte* address, size_t alignment)
        {
            const auto mask = alignment - 1;
            return reinterpret_cast<std::byte*>(reinterpret_cast<uintptr_t>(address + mask) & ~mask);
        }

    public:
        Pool(Pools<Size>* pools, size_t allocationSize, size_t alignment) : m_header{ .m_pools = pools }
        {
            std::byte* end = std::end(m_data);
            std::byte* ptr = Align(std::begin(m_data) + sizeof(Prefix), alignment);
            Element** previousNext = &m_header.m_firstFree;

            for (;;)
            {
                auto* element = reinterpret_cast<Element*>(ptr);
                auto* next = Align(ptr + allocationSize + sizeof(Prefix), alignment);
                if (next > end)
                {
                    *previousNext = nullptr;
                    break;
                }
                reinterpret_cast<Prefix*>(ptr)[-1].m_offset =
                    static_cast<OffsetType>(ptr - reinterpret_cast<std::byte*>(&m_header));
                *previousNext = element;
                previousNext = &element->m_next;
                ptr = next;
            }
        }
        void* Allocate(const std::scoped_lock<std::mutex>&)
        {
            Element* element = m_header.m_firstFree;
            if (element)
            {
                m_header.m_firstFree = element->m_next;
                ++m_header.m_useCount;
            }
            return element;
        }

        static void Free(const std::scoped_lock<std::mutex>&, void* address)
        {
            auto element = reinterpret_cast<Element*>(address);
            auto* header = reinterpret_cast<Header*>(
                reinterpret_cast<std::byte*>(address) - reinterpret_cast<Prefix*>(address)[-1].m_offset);
            element->m_next = std::exchange(header->m_firstFree, element);
            --header->m_useCount;
        }

        static Pool* GetPool(void* address)
        {
            auto offset = reinterpret_cast<Prefix*>(address)[-1].m_offset;
            if (!offset)
            {
                return nullptr;
            }
            return reinterpret_cast<Pool*>(reinterpret_cast<std::byte*>(address) - offset);
        }

        static Pools<Size>* GetPools(void* address)
        {
            auto* pool = reinterpret_cast<Pool*>(
                reinterpret_cast<std::byte*>(address) - reinterpret_cast<Prefix*>(address)[-1].m_offset);
            return pool->m_header.m_pools;
        }

        auto GetUseCount() const { return m_header.m_useCount; }
        bool IsExhausted() const { return m_header.m_firstFree == nullptr; }
    };

    template<size_t Size>
    class Pools
    {
        using PoolType = Pool<Size>;
        std::list<PoolType> m_pools{};
        std::mutex m_mutex{};

    public:
        void* Allocate(size_t allocationSize, size_t alignment)
        {
            std::scoped_lock<std::mutex> guard{ m_mutex };
            for (auto& p : m_pools)
            {
                if (void* address = p.Allocate(guard))
                {
                    if (p.IsExhausted())
                    {
                        std::cerr << "Pool is exhausted\n";
                    }
                    return address;
                }
            }
            auto& p = m_pools.emplace_back(this, allocationSize, alignment);
            return p.Allocate(guard);
        }

        bool Free(void* address)
        {
            Pool<Size>* pool = Pool<Size>::GetPool(address);
            if (!pool)
            {
                return false;
            }
            std::scoped_lock<std::mutex> guard{ m_mutex };
            bool wasExhausted = pool->IsExhausted();
            Pool<Size>::Free(guard, address);
            if (wasExhausted && !pool->IsExhausted())
            {
                std::cerr << "Pool is no longer exhausted\n";
            }
            return true;
        }
    };

    template<std::size_t Size>
    class AllPools
    {
        std::map<std::pair<decltype(sizeof(int)), decltype(alignof(int))>, Pools<Size>> m_pools;

    public:
        Pools<Size>& GetPool(decltype(sizeof(int)) allocationSize, decltype(alignof(int)) alignment)
        {
            return m_pools[std::make_pair(allocationSize, alignment)];
        }
        bool HasPool(decltype(sizeof(int)) allocationSize, decltype(alignof(int)) alignment)
        {
            return m_pools.find(std::make_pair(allocationSize, alignment)) != m_pools.end();
        }
        static AllPools& Instance() {
            static AllPools pools_{};
            return pools_;
        }
    };

    consteval inline size_t AlignedSize(decltype(sizeof(int)) size, decltype(alignof(int)) align)
    {
        return (size + align - 1) & ~(align - 1);
    }


    template<class T>
    class Allocator
    {
    public:
        using pointer = T*;
        using const_pointer = const T*;
        using void_pointer = void*;
        using const_void_pointer = const void*;
        using value_type = T;
        using size_type = std::size_t;
        using difference_type = std::ptrdiff_t;

        using RealAllocator = Pools<2040>;
        using SizeType = decltype(sizeof(T));
        using AlignType = decltype(alignof(T));

        RealAllocator& alloc_{ AllPools<2040U>::Instance().GetPool(sizeof(T), alignof(T)) };

    public:
        Allocator() = default;
        Allocator(const Allocator& copy) noexcept {}
        template<typename Other>
        Allocator(const Allocator<Other>& copy) noexcept
        {
        }
        Allocator& operator=(const Allocator& copy) noexcept = delete;
        Allocator(Allocator&& donor) noexcept {}
        Allocator& operator=(Allocator&& donor) noexcept = delete;
        ~Allocator() = default;

        static inline constexpr size_t maxAllocationSize = 400U;
        [[nodiscard]] pointer allocate(std::size_t count)
        {
            if (size_t alignSize = AlignedSize(sizeof(T), alignof(T)); alignSize < maxAllocationSize)
            {
                if (count == 1U)
                {
                    auto* p = alloc_.Allocate(sizeof(T), alignof(T));
                    return static_cast<pointer>(p);
                }
                else if (size_t allocationSize = alignSize * count;
                         allocationSize < maxAllocationSize &&
                         AllPools<2040U>::Instance().HasPool(allocationSize, alignof(T)))
                {
                    auto& pool = AllPools<2040U>::Instance().GetPool(allocationSize, alignof(T));
                    auto* p = pool.Allocate(allocationSize, alignof(T));
                    return static_cast<pointer>(p);
                }
            }


            size_t prefixSize = AlignedSize(sizeof(MaxOffsetType), alignof(T));
            size_t rawSize = (count * sizeof(T)) + prefixSize;
            void* address = ::operator new(rawSize);
            auto* result = reinterpret_cast<std::byte*>(address) + prefixSize;
            reinterpret_cast<unsigned int*>(result)[-1] = 0UL;
            return reinterpret_cast<pointer>(result);
        }
        pointer allocate(size_t count, pointer cvt)
        {
            return allocate(count);
        }
        void deallocate(pointer p, std::size_t count)
        {
            if (!alloc_.Free(p))
            {
                void* address = static_cast<void*>(reinterpret_cast<std::byte*>(p) - AlignedSize(sizeof(MaxOffsetType), alignof(T)));
                ::operator delete(address);
            }
        }
        size_type max_size()
        {
            size_t allocationSize = ((sizeof(T) + sizeof(Pool<2048U>::Prefix) + alignof(T) - 1) & ~(alignof(T) - 1));
            return 2048U / allocationSize;
        }
    };
} // namespace tbd