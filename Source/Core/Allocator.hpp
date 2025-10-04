#pragma once
#include <atomic>
#include <memory>
#include <stdexcept>
namespace Foundation::Core {
	using size_type = std::size_t;
	using pointer = void*;
	inline uintptr_t AlignUp(uintptr_t value, uintptr_t alignment) {
        return (value + alignment - 1) & ~(alignment - 1);
	}
	inline uintptr_t AlignDown(uintptr_t value, uintptr_t alignment) {
		return (value) & ~(alignment - 1);
	}
    /**
     * @brief A memory arena allocated from an Allocator
     */
    struct Arena {
        pointer memory;
        size_type size;
    };
    /**
     * @brief General Purpose Allocator (GPA) interface
     */
    class Allocator {
	public:
        virtual ~Allocator() = default;
		virtual pointer Allocate(size_type size, size_t alignment = alignof(std::max_align_t)) = 0;
		virtual void Deallocate(pointer ptr, size_type size) = 0;
        virtual void Deallocate(pointer ptr) = 0;
        virtual pointer Reallocate(pointer ptr, size_type new_size, size_t alignment) = 0;

        Arena AllocateArena(size_type size, size_t alignment) { return { Allocate(size, alignment), size }; }
        Arena AllocateArena(size_type size) { return { Allocate(size), size }; }
        void DeallocateArena(Arena arena) {
            if (arena.memory) 
                Deallocate(arena.memory, arena.size);
        }
        Allocator* Ptr() { return this; }
	};
    /**
     * @brief RAII wrapper for an arena allocated from an Allocator
     */
    struct ScopedArena {
        Allocator* resource;
        Arena arena;
        ScopedArena(Allocator* res, size_t size, size_t alignment = alignof(std::max_align_t)) :
            resource(res), arena(res->AllocateArena(size, alignment)) {};
        ~ScopedArena() { resource->DeallocateArena(arena); }
        constexpr operator Arena() const { return arena; }
        constexpr operator bool() const noexcept { return arena.memory != nullptr; }
    };
    constexpr size_t kDefaultStackArenaSize = 12 * 1024; // 12 KiB
    /**
     * @brief A fixed-size stack memory arena
     */
    template<size_t Size = kDefaultStackArenaSize> struct StackArena {
        alignas(std::max_align_t) std::byte data[Size];
        constexpr operator Arena() { return { reinterpret_cast<void*>(data), Size }; }
        constexpr operator Arena() const { return { reinterpret_cast<void*>(data), Size }; }
    };

    /**
     * @brief `std::allocator` adaptor for @ref Foundation::Core::Allocator
     *
     * Construction without a @ref Foundation::Core::Allocator pointer is disallowed, and will result in a compile-time error.
     * For STL types that require default-constructible allocators, use `StlAllocator<void>` and pass the resource explicitly
     *
     * Rebind construction is supported.
     *
     * Using this with e.g. @ref Foundation::Core::Vector can be done as follows:
     * @code{.cpp}
     * Allocator* resource = ...;
     * Vector<int> vector(resource);
     * @endcode
     */
    template <typename T = void>
    class StlAllocator {
        Allocator* mResource;
    public:
        using value_type = T;
        using size_type = std::size_t;
        using difference_type = std::ptrdiff_t;
        using pointer = T*;
        using const_pointer = const T*;
        using reference = T&;
        using const_reference = const T&;

        template<typename U> friend class StlAllocator; // Rebind ctor
        template<typename U> struct Rebind { using other = StlAllocator<U>; };
        StlAllocator(Allocator* resource) noexcept : mResource(resource) {}            
        template<typename U>
        StlAllocator(const StlAllocator<U>& other) noexcept : mResource(other.mResource) {}

        pointer allocate(size_type n) {
            return static_cast<pointer>(mResource->Allocate(n * sizeof(T), alignof(T)));
        }
        void deallocate(pointer p, size_type n) noexcept {
            mResource->Deallocate(p, n * sizeof(T));
        }
        void deallocate(pointer p) noexcept { 
            mResource->Deallocate(p, sizeof(T)); 
        }
		// Allocators are deemed equal if they point to the same resource
        friend bool operator==(const StlAllocator& lhs, const StlAllocator& rhs) noexcept {
            return lhs.mResource == rhs.mResource;
        }
        friend bool operator!=(const StlAllocator& lhs, const StlAllocator& rhs) noexcept {
            return !(lhs == rhs);
        }          
    };

    /**
     * @brief Custom deleter for @ref Foundation::Core::UniquePtr and @ref Foundation::Core::SharedPtr that uses a @ref Foundation::Core::Allocator to deallocate memory.
     */
    template <typename T>
    struct StlDeleter {
        Allocator* mResource;
        void operator()(T* ptr) noexcept {
            if (ptr) {
                std::destroy_at(ptr);
                mResource->Deallocate(ptr, sizeof(T));                
            }
        }
    };

    /**
     * @brief `std::unique_ptr` with custom deleter that uses a @ref Foundation::Core::Allocator to deallocate memory.
     *
     * Construction without a @ref Foundation::Core::Allocator pointer is disallowed, and will result in a compile-time error.
     */
    template<typename T>
    using UniquePtr = std::unique_ptr<T, StlDeleter<T>>;

    /**
     * @brief Helper function for constructing a pinned resource with a @ref Foundation::Core::Allocator.
     *
     * @tparam Base is the base class type be templated on.
     * @tparam Derived can be a subclass of Base, with destruction handled correctly.
     */
    template <typename Base, typename Derived, typename ...Args>
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
    /**
     * @brief Convenience wrapper for calling @ref ConstructUniqueBase when Base and Derived are the same type.
     *
     * @tparam T is the class type to be templated on.
     */
    template <typename T, typename ...Args>
    UniquePtr<T> ConstructUnique(Allocator* resource, Args&& ...args) {
        return ConstructUniqueBase<T, T>(resource, std::forward<Args>(args)...);
    }

    /**
     * @brief `std::shared_ptr` with custom deleter that uses a @ref Foundation::Core::Allocator to deallocate memory.
     */
    template<typename T>
    using SharedPtr = std::shared_ptr<T>;

    /**
     * @brief Helper function for constructing a ref-counted resource with a @ref Foundation::Core::Allocator.
     *
     * @tparam Base is the base class type be templated on.
     * @tparam Derived can be a subclass of Base, with destruction handled correctly.
     */
    template<typename Base, typename Derived, typename ...Args>
    SharedPtr<Base> ConstructSharedBase(Allocator* resource, Args&& ...args) {
        auto up = ConstructUnique<Derived>(resource, std::forward<Args>(args)...);
        return { up.release(), StlDeleter<Base>{ resource } };
    }

    /**
     * @brief Convenience wrapper for calling @ref ConstructSharedBase when Base and Derived are the same type.
     *
     * @tparam T is the class type to be templated on.
     */
    template <typename T, typename ...Args>
    SharedPtr<T> ConstructShared(Allocator* resource, Args&& ...args) {
        return ConstructSharedBase<T, T>(resource, std::forward<Args>(args)...);
    }            
}
