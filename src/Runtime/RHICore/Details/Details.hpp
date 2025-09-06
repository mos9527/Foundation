#pragma once
#include <Core/Core.hpp>
#include <Core/Container/FreeList.hpp>

/**
 * @brief Low-level Rendering Hardware Interface (RHI) abstractions.
 */
namespace Foundation::RHI {
    using Handle = uint64_t;
    constexpr static Handle kInvalidHandle = static_cast<Handle>(-1);
    /**
     * @brief Base class for all RHI objects.
     * RHI Objects are non-copyable, non-movable (pinned), and must be derived from this class.
     */
    class RHIObject {
    public:
        RHIObject() = default;
        RHIObject(RHIObject const&) = delete;
        RHIObject& operator=(const RHIObject&) = delete;
        RHIObject(RHIObject&&) = delete;
        RHIObject& operator=(RHIObject&&) = delete;

        virtual ~RHIObject() = default;
    };
    /**
     * @brief Provides type traits for types derived from RHIObject.
     */
    template<typename Factory, std::derived_from<RHIObject> T>
    struct RHIObjectTraits;
    /**
     * @brief Handle type for RHI Objects.  
     *
     * RHIHandle<Factory, T> are trivially copiable objects that provide a view into the underlying RHIObject storage.
     * When a Factory goes out of scope, all underlying RHIObjects are destroyed.
     *
     * The behavior is undefined to use a Handle after it's Factory has been destroyed or the resource
     * it refers to has been destroyed.
     */
    template<typename Factory, typename T> class RHIHandle {
    public:
        ///
        Factory* m_factory{ nullptr };
        Handle m_handle{ kInvalidHandle };
        /**
         * @brief Retrieves the underlying RHIObject pointer.
         * It is undefined behavior to use the returned pointer after the underlying resource has been destroyed.
         */
        /// <typeparam name="U">Pointer type to retrieve as. U is required to be castable from T</typeparam>                
        template<typename U = T> U* Get() const {
            CHECK(IsValid() && "RHIHandle::Get called on an invalid handle");
            auto ptr = RHIObjectTraits<Factory, T>::Get(m_factory, m_handle);
            return static_cast<U*>(ptr);
        }
        T* operator->() const {
            return Get();
        }

        constexpr Handle operator()() const { return m_handle; }
        constexpr operator bool() const noexcept { return IsValid(); }
        bool operator==(const RHIHandle& other) const { return m_factory == other.m_factory && m_handle == other.m_handle; }

        bool IsValid() const { return m_factory != nullptr && m_handle != kInvalidHandle; }
        bool IsFrom(const Factory* factory) const { return m_factory == factory; }
        void Invalidate() { m_factory = nullptr, m_handle = kInvalidHandle; }
    };
    /**
     * @brief Scoped move-only RAII handle wrapper for RHI Objects.
     */
    template<typename Factory, typename T> class RHIScopedHandle : public RHIHandle<Factory, T> {
    public:
        using RHIHandle<Factory, T>::m_factory;
        using RHIHandle<Factory, T>::m_handle;
        using RHIHandle<Factory, T>::Get;
        using RHIHandle<Factory, T>::IsValid;
        using RHIHandle<Factory, T>::IsFrom;
        using RHIHandle<Factory, T>::Invalidate;

        RHIScopedHandle() {};
        RHIScopedHandle(Factory* factory, Handle handle) : RHIHandle<Factory, T>(factory, handle) {}
        RHIScopedHandle(RHIScopedHandle&& other) noexcept
            : RHIHandle<Factory, T>(std::move(other)) {
            other.Invalidate();
        }
        RHIScopedHandle& operator=(RHIScopedHandle&& other) noexcept {
            if (this != &other) {
                m_factory = other.m_factory;
                m_handle = other.m_handle;
                other.Invalidate();
            }
            return *this;
        }
        /**
         * @brief Returns a non-owning view of the underlying RHIHandle.
         */
        RHIHandle<Factory, T> View() const {
            return *this;
        }
        /**
         * @brief Releases the underlying RHIHandle, invalidating the scoped handle.
         * NOTE: This may leak the resource if not properly managed afterwards.
         */
        RHIHandle<Factory, T> Release() {
            RHIHandle<Factory, T> handle = *this;
            Invalidate();
            return handle;
        }
        /**
         * @brief Destructs the underlying RHIObject, if valid, and invalidates the scoped handle.
         */
        void Reset() {
            if (IsValid()) {
                RHIObjectTraits<Factory, T>::Destroy(m_factory, m_handle);
                Invalidate();
            }            
        }
        RHIScopedHandle(const RHIScopedHandle&) = delete;
        RHIScopedHandle& operator=(const RHIScopedHandle&) = delete;
        ~RHIScopedHandle() {
            if (IsValid())
                RHIObjectTraits<Factory, T>::Destroy(m_factory, m_handle);
        }
    };
    /**
     * @brief Storage/Object dereference facility for RHI Objects
     */
    template<typename Base = RHIObject> class RHIObjectStorage {
        Core::Allocator* m_allocator;
        Core::FreeList<Handle, Core::UniquePtr<Base>> m_objects;
    public:
        RHIObjectStorage(Core::Allocator* allocator) : m_allocator(allocator), m_objects(allocator) {};
        RHIObjectStorage(Core::Allocator* allocator, size_t reserve_size) :
            m_allocator(allocator), m_objects(allocator, reserve_size) {
        };
        /**
         * @brief Creates specified RHIObject of derived type T and retrieves its handle
         */
        /// <returns>The newly allocated Handle of the said RHIObject.</returns>
        template<typename U, typename ...Args> Handle CreateObject(Args&&... args) {
            auto [handle, value] = m_objects.allocate();
            value = Core::ConstructUniqueBase<Base, U>(m_allocator, std::forward<Args>(args)...);
            return handle;
        }
        /**
         * @brief Retrieves the raw pointer to the object within the storage.
         */
        /// <typeparam name="U">Pointer type to cast to.</typeparam>
        /// <returns>The raw pointer.</returns>
        template<typename U = Base> U* GetObjectPtr(Handle handle) const {
            if (!m_objects.contains(handle))
                throw std::out_of_range("invalid handle");
            return static_cast<U*>(m_objects.at(handle).get());
        }
        /**
         * @brief Destroys the object associated with the given handle, and frees the handle for reuse.
         */
        /// <param name="handle"></param>
        inline void DestroyObject(Handle handle) {
            m_objects.free(handle);
        }
        /**
         * @brief Removes all elements from the m_objects container.
         * Using handles acquired earlier will result in undefined behavior.
         */
        inline void Clear() {
            m_objects.clear();
        }
        /**
         * @brief Number of objects currently stored in the storage.
         */
        inline size_t Allocation() const {
            return m_objects.allocation() > 0;
        }
    };
}


namespace Foundation::RHI {
    class RHIApplication;
    template<typename T> using RHIApplicationScopedObjectHandle = RHIScopedHandle<RHIApplication, T>;
    template<typename T> using RHIApplicationObjectHandle = RHIHandle<RHIApplication, T>;

    class RHIDevice;
    template<typename T> using RHIDeviceScopedObjectHandle = RHIScopedHandle<RHIDevice, T>;
    template<typename T> using RHIDeviceObjectHandle = RHIHandle<RHIDevice, T>;
}
