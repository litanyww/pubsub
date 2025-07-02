#pragma once

#include <bit>
#include <cstdint>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

namespace tbd
{
    using MaxOffsetType = unsigned int;
    using SizeType = decltype(sizeof(int));
    using AlignType = decltype(alignof(int));

    consteval std::size_t AlignedSize(SizeType allocationSize, AlignType alignment)
    {
        return (allocationSize + alignment - 1) & ~(alignment - 1);
    }
    constexpr std::size_t OffsetSize(std::size_t blockSize)
    {
        if (blockSize < 0x10000ULL)
        {
            return 2U;
        }
        if (blockSize < 0x1'0000'0000ULL)
        {
            return 4U;
        }
        return 8U;
    }

    template<typename T>
    typename std::list<T>::iterator GetListIterator(T& value)
    {
        typename std::list<T>::iterator result{};
        void* null{};
        return std::bit_cast<typename std::list<T>::iterator>(
            std::bit_cast<std::byte*>(&value) -
            std::bit_cast<uintptr_t>(&std::bit_cast<typename std::list<T>::iterator>(null).operator*()));
    }

    inline std::byte* Align(std::byte* address, std::size_t alignment)
    {
        const auto mask = alignment - 1;
        return reinterpret_cast<std::byte*>(reinterpret_cast<uintptr_t>(address + mask) & ~mask);
    }

    template<std::size_t Size>
    class Pools
    {
        static inline constexpr std::size_t offsetSize = OffsetSize(Size);
        using OffsetType = std::conditional_t<offsetSize == 2, unsigned short, std::conditional_t<offsetSize == 4, unsigned int, unsigned long long>>;
        public: // temporary
        class Pool
        {
            class Header
            {
                Pools* m_pools{};
                unsigned int m_useCount{};
                OffsetType m_firstFree{};

            public:
                Header(Pools* pools, SizeType allocationSize, AlignType alignment) : m_pools{ pools }
                {
                    auto* const start = reinterpret_cast<std::byte*>(this);
                    auto* ptr = Align(start + sizeof(Header) + sizeof(OffsetType), alignment);
                    const auto* const end = start + Size;
                    OffsetType* previousNext = &m_firstFree;

                    for (;;)
                    {
                        auto* next = Align(ptr + allocationSize + sizeof(OffsetType), alignment);
                        if (next > end)
                        {
                            *previousNext = 0U;
                            break;
                        }
                        reinterpret_cast<OffsetType*>(ptr)[-1] = ptr - start;
                        *previousNext = ptr - start;
                        previousNext = reinterpret_cast<OffsetType*>(ptr);
                        ptr = next;
                    }
                }

                bool IsExhausted() const { return m_firstFree == 0U; }
                size_t GetUseCount() const { return m_useCount; }

                void* GetFirst()
                {
                    if (!m_firstFree)
                    {
                        return nullptr;
                    }

                    ++m_useCount;
                    auto* result = reinterpret_cast<std::byte*>(this) + m_firstFree;
                    m_firstFree = *reinterpret_cast<OffsetType*>(result);
                    return result;
                }

                void Return(void* address)
                {
                    --m_useCount;
                    *reinterpret_cast<OffsetType*>(address) = m_firstFree;
                    m_firstFree = reinterpret_cast<const std::byte*>(address) - reinterpret_cast<const std::byte*>(this);
                }
            };


            Header m_header;
            std::byte m_data[Size - sizeof(m_header)];
            static_assert(sizeof(m_data) > 0UL);

        public:
            Pool(Pools* pools, std::size_t allocationSize, std::size_t alignment) :
                m_header{ pools, allocationSize, alignment }
            {
            }
            void* Allocate(const std::scoped_lock<std::mutex>&)
            {
                return m_header.GetFirst();
            }

            void Free(const std::scoped_lock<std::mutex>&, void* address)
            {
                m_header.Return(address);
            }

            static Pool* GetPool(void* address)
            {
                if (auto offset = reinterpret_cast<OffsetType*>(address)[-1]; offset != 0U)
                {
                    return reinterpret_cast<Pool*>(reinterpret_cast<std::byte*>(address) - offset);
                }
                return nullptr;
            }

            static Pools* GetPools(void* address)
            {
                auto* pool = reinterpret_cast<Pool*>(
                    reinterpret_cast<std::byte*>(address) - reinterpret_cast<OffsetType*>(address)[-1]);
                return reinterpret_cast<Pools*>(pool->m_header.m_pools);
            }

            auto GetUseCount() const { return m_header.GetUseCount(); }
            bool IsExhausted() const { return m_header.IsExhausted(); }
        };
        std::list<Pool> m_pools{};
        std::list<Pool> m_empty{};
        std::mutex m_mutex{};

    public:
        void* Allocate(std::size_t allocationSize, std::size_t alignment)
        {
            std::scoped_lock<std::mutex> guard{ m_mutex };
            for (auto it = m_pools.begin(); it != m_pools.end(); ++it)
            {
                if (void* address = it->Allocate(guard))
                {
                    if (it->IsExhausted())
                    {

                        std::cerr << "Moving exhausted pool to end " << allocationSize << " " << alignment << "\n";
                        m_pools.splice(m_pools.end(), m_pools, it);
                    }
                    return address;
                }
            }
            if (!m_empty.empty())
            {
                auto it = m_empty.begin();
                void* address = it->Allocate(guard);
                m_pools.splice(m_pools.begin(), m_empty, it);
                std::cerr << "Moving empty pool back into play " << allocationSize << " " << alignment << "\n";
                return address;
            }

            std::cerr << "creating new pool " << allocationSize << " " << alignment << "\n";
            auto& p = m_pools.emplace_front(this, allocationSize, alignment);
            return p.Allocate(guard);
        }

        bool Free(void* address)
        {
            Pool* pool = Pool::GetPool(address);
            if (!pool)
            {
                return false;
            }
            std::scoped_lock<std::mutex> guard{ m_mutex };
            pool->Free(guard, address);
            if (pool->GetUseCount() == 0U)
            {
                typename std::list<Pool>::iterator it = GetListIterator(*pool);
                std::cerr << "Pool is now empty\n";
                if (m_empty.empty())
                {
                    std::cerr << "putting pool into empty pool\n";
                    m_empty.splice(m_empty.end(), m_pools, it);
                }
                else
                {
                    std::cerr << "dropping empty pool\n";
                    m_pools.erase(it);
                }
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

        static inline constexpr std::size_t maxAllocationSize = 400U;
        [[nodiscard]] pointer allocate(std::size_t count)
        {
            if (std::size_t alignSize = AlignedSize(sizeof(T), alignof(T)); alignSize < maxAllocationSize)
            {
                if (count == 1U)
                {
                    auto* p = alloc_.Allocate(sizeof(T), alignof(T));
                    return static_cast<pointer>(p);
                }
                else if (std::size_t allocationSize = alignSize * count;
                         allocationSize < maxAllocationSize &&
                         AllPools<2040U>::Instance().HasPool(allocationSize, alignof(T)))
                {
                    auto& pool = AllPools<2040U>::Instance().GetPool(allocationSize, alignof(T));
                    auto* p = pool.Allocate(allocationSize, alignof(T));
                    return static_cast<pointer>(p);
                }
            }


            std::size_t prefixSize = AlignedSize(sizeof(MaxOffsetType), alignof(T));
            std::size_t rawSize = (count * sizeof(T)) + prefixSize;
            void* address = ::operator new(rawSize);
            auto* result = reinterpret_cast<std::byte*>(address) + prefixSize;
            reinterpret_cast<unsigned int*>(result)[-1] = 0UL;
            return reinterpret_cast<pointer>(result);
        }
        pointer allocate(std::size_t count, pointer cvt)
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
            std::size_t allocationSize = ((sizeof(T) + OffsetSize(2048U) + alignof(T) - 1) & ~(alignof(T) - 1));
            return 2048U / allocationSize;
        }
    };
} // namespace tbd