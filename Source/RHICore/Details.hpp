#pragma once
#include <Core/Core.hpp>
#include <Core/Pool.hpp>

/**
 * @brief Low-level Rendering Hardware Interface (RHI) abstractions.
 */
namespace Foundation::RHI {
    using Handle = uint64_t;
    constexpr static Handle kInvalidHandle = static_cast<Handle>(-1);
    /**
     * @brief Base class for all RHI objects.
     *
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
        Factory* mFactory{ nullptr };
        Handle mHandle{ kInvalidHandle };
        /**
         * @brief Retrieves the underlying RHIObject pointer.
         * It is undefined behavior to use the returned pointer after the underlying resource has been destroyed.
         * @tparam U Pointer type to retrieve as. U is required to be castable from T
         */
        template<typename U = T> [[nodiscard]] U* Get() const {
            CHECK(IsValid() && "RHIHandle::Get called on an invalid handle");
            auto ptr = RHIObjectTraits<Factory, T>::Get(mFactory, mHandle);
            return static_cast<U*>(ptr);
        }
        T* operator->() const {
            return Get();
        }

        constexpr Handle operator()() const { return mHandle; }
        constexpr operator bool() const noexcept { return IsValid(); }
        bool operator==(const RHIHandle& other) const { return mFactory == other.mFactory && mHandle == other.mHandle; }

        [[nodiscard]] bool IsValid() const { return mFactory != nullptr && mHandle != kInvalidHandle; }
        bool IsFrom(const Factory* factory) const { return mFactory == factory; }
        void Invalidate() { mFactory = nullptr, mHandle = kInvalidHandle; }
    };
    /**
     * @brief Scoped move-only RAII handle wrapper for RHI Objects.
     */
    template<typename Factory, typename T> class RHIScopedHandle : public RHIHandle<Factory, T> {
    public:
        using RHIHandle<Factory, T>::mFactory;
        using RHIHandle<Factory, T>::mHandle;
        using RHIHandle<Factory, T>::Get;
        using RHIHandle<Factory, T>::IsValid;
        using RHIHandle<Factory, T>::IsFrom;
        using RHIHandle<Factory, T>::Invalidate;

        RHIScopedHandle() = default;
        RHIScopedHandle(Factory* factory, Handle handle) : RHIHandle<Factory, T>(factory, handle) {}
        RHIScopedHandle(RHIScopedHandle&& other) noexcept
            : RHIHandle<Factory, T>(std::move(other)) {
            other.Invalidate();
        }
        RHIScopedHandle& operator=(RHIScopedHandle&& other) noexcept {
            if (this != &other) {
                mFactory = other.mFactory;
                mHandle = other.mHandle;
                other.Invalidate();
            }
            return *this;
        }
        /**
         * @brief Returns a non-owning view of the underlying RHIHandle.
         */
        [[nodiscard]] RHIHandle<Factory, T> View() const {
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
                RHIObjectTraits<Factory, T>::Destroy(mFactory, mHandle);
                Invalidate();
            }            
        }
        RHIScopedHandle(const RHIScopedHandle&) = delete;
        RHIScopedHandle& operator=(const RHIScopedHandle&) = delete;
        ~RHIScopedHandle() {
            if (IsValid())
                RHIObjectTraits<Factory, T>::Destroy(mFactory, mHandle);
        }
    };
    /**
     * @brief Storage/Object dereference facility for RHI Objects
     */
    template<typename Base = RHIObject> class RHIObjectPool {
        Core::Allocator* mAllocator;
        Core::Pool<Handle, Core::UniquePtr<Base>> mObjects;
    public:
        RHIObjectPool(Core::Allocator* allocator) :
            mAllocator(allocator), mObjects(allocator) {
        };
        /**
         * @brief Creates specified RHIObject of derived type T and retrieves its handle
         * @returns A handle to the newly created object.
         */
        template<typename U, typename ...Args> Handle CreateObject(Args&&... args) {
            auto [handle, value] = mObjects.PopPair();
            value = Core::ConstructUniqueBase<Base, U>(mAllocator, std::forward<Args>(args)...);
            return handle;
        }
        /**
         * @brief Retrieves the raw pointer to the object within the storage.
         * @tparam U Pointer type to cast to.
         * @returns The raw pointer.
         */
        template<typename U = Base> U* GetObjectPtr(Handle handle) const {
            if (!mObjects.Contains(handle))
                throw std::out_of_range("invalid handle");
            return static_cast<U*>(mObjects.At(handle).get());
        }
        /**
         * @brief Destroys the object associated with the given handle, and frees the handle for reuse.
         */
        void DestroyObject(Handle handle) {
            mObjects.Free(handle);
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
