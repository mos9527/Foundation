#pragma once
#include <Core/Core.hpp>
#include <atomic>
#include <memory>
#include <stdexcept>

namespace Foundation {
	namespace Core {		
		inline uintptr_t AlignUp(uintptr_t value, uintptr_t alignment) {
            return (value + alignment - 1) & ~(alignment - 1);
		}
		inline uintptr_t AlignDown(uintptr_t value, uintptr_t alignment) {
			return (value) & ~(alignment - 1);
		}
        typedef size_t CounterSingleThreaded;
        typedef std::atomic<size_t> CounterMultiThreaded;
		class Allocator {			
		public:
			using size_type = std::size_t;
			using pointer = void*;
            struct Arena {
                pointer memory;
                size_type size;
            };

			virtual pointer Allocate(size_type size) = 0;
			virtual pointer Allocate(size_type size, size_t alignment) = 0;
			virtual void Deallocate(pointer ptr, size_type size) = 0;
            virtual void Deallocate(pointer ptr) = 0;
            virtual pointer Reallocate(pointer ptr, size_type new_size, size_t alignment) = 0;

            inline Arena AllocateArena(size_type size, size_t alignment) { return { Allocate(size, alignment), size }; }
            inline Arena AllocateArena(size_type size) { return { Allocate(size), size }; }
            inline void DeallocateArena(Arena arena) {
                if (arena.memory) 
                    Deallocate(arena.memory, arena.size);
            }
            virtual size_type GetUsedMemory() const noexcept = 0;

            inline Allocator* Ptr() { return this; }
		};
        struct ScopedArena {
            Allocator* resource{ nullptr };
            Allocator::Arena arena;
            ScopedArena(Allocator* res, size_t size, size_t alignment = alignof(std::max_align_t)) :
                resource(res), arena(res->AllocateArena(size, alignment)) {};
            ~ScopedArena() { resource->DeallocateArena(arena); }
            constexpr operator Allocator::Arena() const { return arena; }
            constexpr operator bool() const noexcept { return arena.memory != nullptr; }
        };
        constexpr size_t kDefaultStackArenaSize = 12 * 1024; // 12 KiB
        template<size_t Size = kDefaultStackArenaSize> struct StackArena {
            alignas(std::max_align_t) std::byte data[Size];
            constexpr operator Allocator::Arena() { return { reinterpret_cast<void*>(data), Size }; }
        };
		
		template<typename T = void>
        class StlAllocator {
            Allocator* m_resource;
        public:
            using value_type = T;
            using size_type = std::size_t;
            using difference_type = std::ptrdiff_t;
            using pointer = T*;
            using const_pointer = const T*;
            using reference = T&;
            using const_reference = const T&;

            template<typename U> friend class StlAllocator; // Rebind ctor
            template<typename U> struct rebind { using other = StlAllocator<U>; };
            StlAllocator(Allocator* resource) noexcept : m_resource(resource) {}            
            template<typename U>
            StlAllocator(const StlAllocator<U>& other) noexcept : m_resource(other.m_resource) {}

            pointer allocate(size_type n) {
                return static_cast<pointer>(m_resource->Allocate(n * sizeof(T), alignof(T)));
            }
            void deallocate(pointer p, size_type n) noexcept {
                m_resource->Deallocate(p, n * sizeof(T));
            }
            void deallocate(pointer p) noexcept { 
                m_resource->Deallocate(p, sizeof(T)); 
            }
			// Allocators are deemed equal if they point to the same resource
            friend bool operator==(const StlAllocator& lhs, const StlAllocator& rhs) noexcept {
                return lhs.m_resource == rhs.m_resource;
            }
            friend bool operator!=(const StlAllocator& lhs, const StlAllocator& rhs) noexcept {
                return !(lhs == rhs);
            }          
        };

        template<typename T>
        struct StlDeleter {
            Allocator* m_resource;
            void operator()(T* ptr) noexcept {
                if (ptr) {
                    std::destroy_at(ptr);
                    m_resource->Deallocate(ptr, sizeof(T));                
                }
            }
        };

        template<typename T>
        using UniquePtr = std::unique_ptr<T, StlDeleter<T>>;

        template<typename Base, typename Derived, typename ...Args>
        UniquePtr<Base> ConstructUniqueBase(Allocator* resource, Args&& ...args) {
            auto raw = resource->Allocate(sizeof(Derived), alignof(Derived));
            try {
                Derived* obj = std::construct_at(static_cast<Derived*>(raw), std::forward<Args>(args)...);
                return UniquePtr<Base>(obj, StlDeleter<Base>{ resource });
            }
            catch (...) {
                resource->Deallocate(raw, sizeof(Derived));
                throw;
            }
        }
        template<typename T, typename ...Args>
        UniquePtr<T> ConstructUnique(Allocator* resource, Args&& ...args) {
            return ConstructUniqueBase<T, T>(resource, std::forward<Args>(args)...);
        }
       
        template<typename T>
        using SharedPtr = std::shared_ptr<T>;
        template<typename Base, typename Derived, typename ...Args>
        SharedPtr<Base> ConstructSharedBase(Allocator* resource, Args&& ...args) {
            auto up = ConstructUnique<Derived>(resource, std::forward<Args>(args)...);
            return { up.release(), StlDeleter<Base>{ resource } };
        }
        template<typename T, typename ...Args>
        SharedPtr<T> ConstructShared(Allocator* resource, Args&& ...args) {
            return ConstructSharedBase<T, T>(resource, std::forward<Args>(args)...);
        }        
    }
}
