#pragma once
#include <atomic>
#include <memory>
#include <stdexcept>
namespace Foundation::Core {
	using size_type = std::size_t;
	using pointer = void*;
	constexpr uintptr_t AlignUp(const uintptr_t value, const uintptr_t alignment) {
        return value % alignment ? (value + alignment - value % alignment) : value;
	}
	constexpr uintptr_t AlignDown(const uintptr_t value, const uintptr_t alignment) {
	    return value % alignment ? (value - value % alignment) : value;
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
        virtual void Deallocate(pointer ptr) = 0;
        virtual pointer Reallocate(pointer ptr, size_type new_size, size_t alignment) = 0;

        virtual void QueryBudget(size_t& used, size_t& budget) const = 0;
        virtual size_t QueryHeapUsage() const = 0;

        Arena AllocateArena(size_type size, size_t alignment) { return { Allocate(size, alignment), size }; }
        Arena AllocateArena(size_type size) { return { Allocate(size), size }; }
        void DeallocateArena(Arena arena) {
            if (arena.memory) 
                Deallocate(arena.memory);
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
    struct StlAllocator {
        using value_type = T;
        using size_type = std::size_t;
        using difference_type = std::ptrdiff_t;
        using pointer = T*;
        using const_pointer = const T*;
        using reference = T&;
        using const_reference = const T&;

        Allocator* mResource;

        template<typename U> friend struct StlAllocator; // Rebind ctor
        template<typename U> struct Rebind { using other = StlAllocator<U>; };
        StlAllocator(Allocator* resource) noexcept : mResource(resource) {}            
        template<typename U>
        StlAllocator(const StlAllocator<U>& other) noexcept : mResource(other.mResource) {}

        pointer allocate(size_type n) {
            return static_cast<pointer>(mResource->Allocate(n * sizeof(T), alignof(T)));
        }
        void deallocate(pointer p, size_type n) noexcept {
            mResource->Deallocate(p);
        }
        void deallocate(pointer p) noexcept { mResource->Deallocate(p); }
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
            // A null resource marks a non-owning view: neither destroy nor free. This lets a
            // UniquePtr<T> reference an externally-owned object (e.g. a stack/arena-resident job
            // co-invoked across worker threads) without taking ownership of its lifetime.
            if (ptr && mResource) {
                std::destroy_at(ptr);
                mResource->Deallocate(ptr);
            }
        }
    };
    /**
     * @brief Placement new helper for constructing an object of type Derived (which can be a subclass of Base) using a @ref Foundation::Core::Allocator.
     * @note Using `delete`, `delete[]` on the returned pointer is undefined behaviour. @ref Destruct should ALWAYS
     *       be used for such purposes.
     */
    template <typename Base, typename Derived, typename ...Args>
    Base* ConstructBase(Allocator* resource, Args&& ...args) {
        auto raw = resource->Allocate(sizeof(Derived), alignof(Derived));
        try {
            Derived* obj = std::construct_at(static_cast<Derived*>(raw), std::forward<Args>(args)...);
            return obj;
        }
        catch (...) {
            resource->Deallocate(raw);
            throw;
        }
    }
    /**
     * @brief Convenience placement new with object of type T using a @ref Foundation::Core::Allocator.
     * @note Using `delete`, `delete[]` on the returned pointer is undefined behaviour. @ref Destruct should ALWAYS
     *       be used for such purposes.
     */
    template <typename T, typename ...Args>
    T* Construct(Allocator* resource, Args&& ...args) {
        return ConstructBase<T, T>(resource, std::forward<Args>(args)...);
    }
    /**
     * @brief Convenience destructor for objects allocated with @ref Construct or @ref ConstructBase.
     */
    template <typename T>
    void Destruct(Allocator* resource, T* obj) {
        StlDeleter<T> deleter(resource);
        deleter(obj);
    }
    /**
     * @brief `std::unique_ptr` with custom deleter that uses a @ref Foundation::Core::Allocator to deallocate memory.
     *
     * Construction without a @ref Foundation::Core::Allocator pointer is disallowed, and will result in a compile-time error.
     */
    template<typename T, typename Deleter = StlDeleter<T>>
    using UniquePtr = std::unique_ptr<T, Deleter>;

    /**
     * @brief Helper function for constructing a pinned resource with a @ref Foundation::Core::Allocator.
     *
     * @tparam Base is the base class type be templated on.
     * @tparam Derived can be a subclass of Base, with destruction handled correctly.
     */
    template <typename Base, typename Derived, typename ...Args>
    UniquePtr<Base> ConstructUniqueBase(Allocator* resource, Args&& ...args) {
        Base* obj = ConstructBase<Base, Derived>(resource, std::forward<Args>(args)...);
        return UniquePtr<Base>(obj, StlDeleter<Base>{ resource });
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
    template<typename T>
    using WeakPtr = std::weak_ptr<T>;

    /**
     * @brief Helper function for constructing a ref-counted resource with a @ref Foundation::Core::Allocator.
     *
     * @tparam Base is the base class type be templated on.
     * @tparam Derived can be a subclass of Base, with destruction handled correctly.
     */
    template<typename Base, typename Derived, typename ...Args>
    SharedPtr<Base> ConstructSharedBase(Allocator* resource, Args&& ...args) {
        return std::allocate_shared<Derived>(StlAllocator<Derived>{resource}, std::forward<Args>(args)...);
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

    /** Implemented by @ref AllocatorHeap **/
    extern Allocator* getGlobalAllocator();
}

#define GLOBAL_ALLOC Foundation::Core::getGlobalAllocator()
