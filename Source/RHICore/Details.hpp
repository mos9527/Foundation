#pragma once
#include <Core/AtomicPool.hpp>
/**
 * @brief Low-level Rendering Hardware Interface (RHI) abstractions.
 */
namespace Foundation::RHI
{
    using Handle = uintptr_t;
    constexpr static Handle kInvalidHandle = static_cast<Handle>(-1);
    /**
     * @brief Base class for all RHI objects.
     *
     * RHI Objects are non-copyable, non-movable (pinned), and must be derived from this class.
     */
    class RHIObject
    {
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
    template <typename Factory, std::derived_from<RHIObject> T>
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
    template <typename Factory, typename T>
    class RHIHandle
    {
    public:
        ///
        Factory* mFactory{nullptr};
        Handle mHandle{kInvalidHandle};
        /**
         * @brief Retrieves the underlying RHIObject pointer.
         * It is undefined behavior to use the returned pointer after the underlying resource has been destroyed.
         * @tparam U Pointer type to retrieve as. U is required to be castable from T
         */
        template <typename U = T>
        [[nodiscard]] U* Get() const
        {
            if (!IsValid())
                throw std::runtime_error("RHIHandle::Get called on an invalid handle");
            T* ptr = RHIObjectTraits<Factory, T>::Get(mFactory, mHandle);
            if (!ptr)
                throw std::runtime_error("RHIHandle::Get got nullptr");
            return static_cast<U*>(ptr);
        }
        T* operator->() const { return Get(); }

        constexpr Handle operator()() const { return mHandle; }
        constexpr operator bool() const noexcept { return IsValid(); }
        constexpr T* operator*() noexcept { return Get(); }
        bool operator==(const RHIHandle& other) const { return mFactory == other.mFactory && mHandle == other.mHandle; }

        /**
         * @brief Check if the handle is valid (i.e. associated with a Factory and not kInvalidHandle)
         * @note THIS CHECK IS CONSERVATIVE. A true handle may still refer to a destroyed resource.
         *       Always ensure correct destruction order, possibly with the help of @ref UniquePtr or @ref SharedPtr
         */
        [[nodiscard]] bool IsValid() const { return mFactory != nullptr && mHandle != kInvalidHandle; }
        /**
         * @brief Resets the handle to an invalid state.
         * After calling this, the handle is no longer associated with any Factory or resource.
         */
        void Invalidate() { mFactory = nullptr, mHandle = kInvalidHandle; }
    };
    /**
     * @brief Scoped move-only RAII handle wrapper for RHI Objects.
     */
    template <typename Factory, typename T>
    class RHIScopedHandle : public RHIHandle<Factory, T>
    {
    public:
        using RHIHandle<Factory, T>::mFactory;
        using RHIHandle<Factory, T>::mHandle;
        using RHIHandle<Factory, T>::Get;
        using RHIHandle<Factory, T>::IsValid;
        using RHIHandle<Factory, T>::Invalidate;

        RHIScopedHandle() = default;
        RHIScopedHandle(Factory* factory, Handle handle) : RHIHandle<Factory, T>(factory, handle) {}
        RHIScopedHandle(RHIScopedHandle&& other) noexcept : RHIHandle<Factory, T>(std::move(other))
        {
            other.Invalidate();
        }
        RHIScopedHandle& operator=(RHIScopedHandle&& other) noexcept
        {
            if (this != &other)
            {
                mFactory = other.mFactory;
                mHandle = other.mHandle;
                other.Invalidate();
            }
            return *this;
        }
        /**
         * @brief Returns a non-owning view of the underlying RHIHandle.
         */
        [[nodiscard]] RHIHandle<Factory, T> View() const { return *this; }
        /**
         * @brief Releases the underlying RHIHandle, invalidating the scoped handle.
         * NOTE: This may leak the resource if not properly managed afterward.
         */
        RHIHandle<Factory, T> Release()
        {
            RHIHandle<Factory, T> handle = *this;
            Invalidate();
            return handle;
        }
        /**
         * @brief Destructs the underlying RHIObject, and invalidates the scoped handle.
         */
        void Reset()
        {
            if (IsValid())
            {
                RHIObjectTraits<Factory, T>::Destroy(mFactory, mHandle);
                Invalidate();
            }
        }
        RHIScopedHandle(const RHIScopedHandle&) = delete;
        RHIScopedHandle& operator=(const RHIScopedHandle&) = delete;
        ~RHIScopedHandle()
        {
            if (IsValid())
                RHIObjectTraits<Factory, T>::Destroy(mFactory, mHandle);
        }
    };
    static constexpr size_t kRHIObjectPoolMaxSize = 65536;
    /**
     * @brief Thread-safe handle dereference facility for RHI Objects
     */
    template <typename Base = RHIObject>
    class RHIObjectPool
    {
        using PointerType = Core::UniquePtr<Base>;

        Core::Allocator* mAllocator;
        Core::AtomicPool<PointerType> mPool;

    public:
        RHIObjectPool(Core::Allocator* allocator) : mAllocator(allocator), mPool(kRHIObjectPoolMaxSize, allocator) {}
        /**
         * @brief Creates specified RHIObject of derived type T and retrieves its handle
         * @returns A handle to the newly created object.
         */
        template <typename U, typename... Args>
        Handle CreateObject(Args&&... args)
        {
            Base* obj = Core::ConstructBase<Base, U>(mAllocator, std::forward<Args>(args)...);
            PointerType* pointer = mPool.Allocate(obj, Core::StlDeleter<Base>(mAllocator));
            return reinterpret_cast<Handle>(pointer);
        }
        /**
         * @brief Retrieves the raw pointer to the object within the storage.
         * @tparam U Pointer type to cast to.
         * @returns The raw pointer.
         */
        template <typename U = Base>
        static U* GetObjectPtr(Handle handle) noexcept
        {
            PointerType& pointer = *reinterpret_cast<PointerType*>(handle);
            Base* obj = pointer.get();
            return reinterpret_cast<U*>(obj);
        }
        /**
         * @brief Destroys the object associated with the given handle, and frees the handle for reuse.
         */
        void DestroyObject(Handle handle)
        {
            auto* pointer = reinterpret_cast<PointerType*>(handle);
            mPool.Deallocate(pointer);
        }
    };
} // namespace Foundation::RHI


namespace Foundation::RHI
{
    class RHIApplication;
    template <typename T>
    using RHIApplicationScopedObjectHandle = RHIScopedHandle<RHIApplication, T>;
    template <typename T>
    using RHIApplicationObjectHandle = RHIHandle<RHIApplication, T>;

    class RHIDevice;
    template <typename T>
    using RHIDeviceScopedObjectHandle = RHIScopedHandle<RHIDevice, T>;
    template <typename T>
    using RHIDeviceObjectHandle = RHIHandle<RHIDevice, T>;
} // namespace Foundation::RHI
