#pragma once
#include <Core.hpp>
#include <atomic>
#include <memory>
#include <stdexcept>

namespace Foundation {
	namespace Core {		
		inline void* AlignUp(void* value, size_t alignment) {
			return reinterpret_cast<void*>((reinterpret_cast<size_t>(value) + alignment - 1) & ~(alignment - 1));
		}
		inline void* AlignDown(void* value, size_t alignment) {
			return reinterpret_cast<void*>(reinterpret_cast<size_t>(value) & ~(alignment - 1));
		}
		class Allocator {			
		public:
			using size_type = std::size_t;
			using pointer = void*;

			virtual pointer allocate(size_type size) = 0;
			virtual pointer allocate(size_type size, size_t alignment) = 0;
			virtual void deallocate(pointer ptr) = 0;
			virtual void deallocate(pointer ptr, size_type size) = 0;
		};
		
		template<typename T>
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
                return static_cast<pointer>(m_resource->allocate(n * sizeof(T), alignof(T)));
            }
            void deallocate(pointer p, size_type n) noexcept {
                m_resource->deallocate(p, n * sizeof(T));
            }
            void deallocate(pointer p) noexcept { 
                m_resource->deallocate(p, sizeof(T)); 
            }
			// Allocators are deemed equal if they point to the same resource
            friend bool operator==(const StlAllocator& lhs, const StlAllocator& rhs) noexcept {
                return lhs.m_resource == rhs.m_resource;
            }
            friend bool operator!=(const StlAllocator& lhs, const StlAllocator& rhs) noexcept {
                return !(lhs == rhs);
            }          
        };
    }
}