#pragma once
#include <span>
#include <Platform/Application.hpp>
#include <Core/Core.hpp>
#include <Core/Bits/FreeList.hpp>
#include <Core/Allocator/Allocator.hpp>

namespace Foundation {
    namespace Platform {
        namespace RHI {
            using Handle = uint64_t;
            constexpr static Handle kInvalidHandle = static_cast<Handle>(-1);
            /// <summary>
            /// Base class for all RHI objects.
            /// RHI Objects are non-copyable, non-movable (pinned), and must be derived from this class.
            /// </summary>
            class RHIObject {
            public:
                RHIObject() = default;
                RHIObject(RHIObject const&) = delete;
                RHIObject& operator=(const RHIObject&) = delete;
                RHIObject(RHIObject&&) = delete;
                RHIObject& operator=(RHIObject&&) = delete;
                /// <summary>
                /// Checks whether the object is in a valid state.
                /// </summary>
                /// <returns>True if the object is valid; otherwise, false.</returns>
                virtual bool IsValid() const = 0;
                virtual const char* GetName() const = 0;

                virtual ~RHIObject() = default;
            };
            /// <summary>
            /// Provides type traits for types derived from RHIObject.
            /// </summary>            
            template<std::derived_from<RHIObject> T>
            struct RHIObjectTraits;
            /// <summary>
            /// Handle type for RHI Objects.  
            /// 
            /// RHIHandle<Factory, T> are trivialy copiable objects that provide a view into the underlying RHIObject storage.
            /// When a Factory goes out of scope, all underlying RHIObjects are destroyed.
            ///
            /// The behaviour is undefined to use a Handle after it's Factory has been destroyed or the resource
            /// it refers to has been destroyed.
            /// </summary>
            template<typename Factory, typename T> class RHIHandle {
            public:
                ///
                Factory* m_factory{ nullptr };
                Handle m_handle{ kInvalidHandle };
                /// <summary>
                /// Retrives the underlying RHIObject pointer.
                /// It is undefined behaviour to use the returned pointer after the underlying resource has been destroyed.                
                /// </summary>
                /// <typeparam name="U">Pointer type to retrive as. U is required to be castable from T</typeparam>                
                template<typename U = T> U* Get() const {
                    auto ptr = RHIObjectTraits<T>::Get(m_factory, m_handle);
                    return static_cast<U*>(ptr);
                }
                T* operator->() const {
                    return Get();
                }

                constexpr Handle operator()() const { return m_handle; }
                constexpr operator bool() const noexcept { return IsValid(); }
                bool operator==(const RHIHandle& other) const { return m_factory == other.m_factory && m_handle == other.m_handle; }

                bool IsValid() const { return m_handle != kInvalidHandle; }
                bool IsFrom(const Factory* factory) const { return m_factory == factory; }
                void Invalidate() { m_factory = nullptr, m_handle = kInvalidHandle; }
            };
            /// <summary>
            /// Scoped move-only RAII handle wrapper for RHI Objects.
            /// </summary>           
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
                /// <summary>
                /// Returns a non-owning view of the underlying RHIHandle.
                /// </summary>                
                RHIHandle<Factory, T> View() const {
                    return *this;
                }
                /// <summary>
                /// Releases the underlying RHIHandle, invalidating the scoped handle.
                /// The resource will never be destroyed, and the handle can be used independently.
                /// </summary>               
                RHIHandle<Factory, T> Release() {
                    RHIHandle<Factory, T> handle = *this;
                    Invalidate();
                    return handle;
                }

                RHIScopedHandle(const RHIScopedHandle&) = delete;
                RHIScopedHandle& operator=(const RHIScopedHandle&) = delete;
                ~RHIScopedHandle() {
                    if (IsValid())
                        RHIObjectTraits<T>::Destroy(m_factory, m_handle);
                }
            };
            /// <summary>
            /// Storage/Object dereference faclity for RHI Objects
            /// </summary>
            template<typename Base = RHIObject> class RHIObjectStorage {
                Core::Allocator* m_allocator;
                Core::Bits::FreeDenseMap<Handle, Base> m_objects;
            public:
                RHIObjectStorage(Core::Allocator* allocator) : m_allocator(allocator), m_objects(allocator) {};
                RHIObjectStorage(Core::Allocator* allocator, size_t reserve_size) :
                    m_allocator(allocator), m_objects(allocator, reserve_size) {};
                /// <summary>
                /// Creates specified RHIObject of derived type T and retrives its handle
                /// </summary>
                /// <returns>The newly allocated Handle of the said RHIObject.</returns>
                template<typename U, typename ...Args> Handle CreateObject(Args&&... args) {
                    auto handle = m_objects.pop();
                    auto& value = m_objects.at(handle);
                    value = Core::ConstructUniqueBase<Base, U>(m_allocator, std::forward<Args>(args)...);
                    return handle;
                }
                /// <summary>
                /// Retrives the raw pointer to the object within the storage.                
                /// </summary>
                /// <typeparam name="U">Pointer type to cast to.</typeparam>
                /// <returns>The raw pointer.</returns>
                template<typename U = Base> U* GetObjectPtr(Handle handle) const {
                    if (handle == kInvalidHandle)
                        return nullptr;
                    return static_cast<U*>(m_objects.at(handle).get());
                }
                /// <summary>
                /// Destorys the object associated with the given handle, and frees the handle for reuse.
                /// </summary>
                /// <param name="handle"></param>
                inline void DestroyObject(Handle handle) {
                    m_objects.erase(handle);
                }
                /// <summary>
                /// Removes all elements from the m_objects container.
                /// Using handles acquired earlier will result in undefined behaviour.
                /// </summary>
                inline void Clear() {
                    m_objects.clear();
                }
                /// <summary>
                /// Number of objects currently stored in the storage.
                /// </summary>                
                inline size_t Allocation() const {
                    return m_objects.allocation() > 0;
                }
            };
        }
    }
}


namespace Foundation {
    namespace Platform {
        namespace RHI {
            class RHIApplication;
            template<typename T> using RHIApplicationScopedObjectHandle = RHIScopedHandle<RHIApplication, T>;
            template<typename T> using RHIApplicationObjectHandle = RHIHandle<RHIApplication, T>;

            class RHIDevice;
            template<typename T> using RHIDeviceScopedObjectHandle = RHIScopedHandle<RHIDevice, T>;
            template<typename T> using RHIDeviceObjectHandle = RHIHandle<RHIDevice, T>;
        }
    }
}
