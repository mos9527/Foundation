#pragma once
#include <Platform/Application.hpp>
#include <Core/Core.hpp>
#include <Core/Bits/FreeList.hpp>
#include <Core/Allocator/Allocator.hpp>

namespace Foundation {
    namespace Platform {
        namespace RHI {
            using Handle = uint64_t;
            class RHIObject {
            public:
                RHIObject() = default;
                RHIObject(RHIObject const&) = delete;
                RHIObject& operator=(const RHIObject&) = delete;
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
            /// When a Factory goes out of scope, all Handles are invalidated, and the underlying RHIObjects are destroyed.
            ///
            /// The behaviour is undefined to use a Handle after it's Factory has been destroyed or the resource
            /// it refers to has been destroyed.
            /// </summary>
            template<typename Factory, typename T> class RHIHandle {
            public:
                constexpr static Handle kInvalidHandle = -1;
                ///
                Factory* m_factory{ nullptr };
                Handle m_handle{ kInvalidHandle };
                /// <summary>
                /// Retrieves a pointer to a device object associated with the current handle.
                /// </summary>               
                template<std::derived_from<T> U = T> U* Get() const {
                    auto ptr = RHIObjectTraits<T>::Get(m_factory, m_handle);
                    return static_cast<U*>(ptr);
                }
                T* operator->() const {
                    return Get();
                }
                constexpr Handle operator()() const { return m_handle; }
                bool IsValid() const { return m_handle != kInvalidHandle; }
                constexpr operator bool() const noexcept { return IsValid(); }
                bool IsFrom(const Factory* factory) const { return m_factory == factory; }
                void Invalidate() { m_handle = kInvalidHandle; }
            };
            /// <summary>
            /// Scoped move-only RAII handle wrapper for RHI Objects.
            /// </summary>           
            template<typename Factory, typename T> class RHIScopedHandle {
                Factory* m_factory{ nullptr };
                RHIHandle<Factory, T> m_underlying{};
            public:
                RHIScopedHandle(Factory* factory, Handle handle) : m_underlying{ factory, handle } {}
                RHIScopedHandle(RHIScopedHandle&& other) noexcept
                    : m_factory(other.m_factory), m_underlying(std::move(other.m_underlying)) {
                    other.m_factory = nullptr;
                    other.m_underlying.Invalidate();
                }
                /// <summary>
                /// Constructs a RHIHandle as a view to the underlying RHIHandle
                /// It is undefined behaviour to use the created handle after the RHIScopedHandle has been destroyed.            
                /// </summary>
                RHIHandle<Factory, T> Get() const {
                    return m_underlying;
                }
                RHIHandle<Factory, T> operator()() const {
                    return Get();
                }
                RHIScopedHandle(const RHIScopedHandle&) = delete;
                RHIScopedHandle& operator=(const RHIScopedHandle&) = delete;
                ~RHIScopedHandle() {
                    if (m_factory && m_underlying)
                        RHIObjectTraits<T>::Destroy(m_factory, m_underlying.m_handle);
                }
            };
            /// <summary>
            /// Storage/Object dereference faclity for RHI Objects
            /// </summary>
            class RHIObjectStorage {
                Core::Allocator* m_allocator;
                Core::Bits::FreeDenseMap<Handle, RHIObject> m_objects;
            public:
                RHIObjectStorage(Core::Allocator* allocator) : m_allocator(allocator), m_objects(allocator) {};
                /// <summary>
                /// Creates specified RHIObject of derived type T and retrives its handle
                /// </summary>
                /// <returns>The newly allocated Handle of the said RHIObject.</returns>
                template<typename T, typename ...Args> Handle CreateObject(Args&&... args) {
                    auto handle = m_objects.pop();
                    auto& value = m_objects.at(handle);
                    value = Core::ConstructUniqueBase<RHIObject, T>(m_allocator, std::forward<Args>(args)...);
                    return handle;
                }
                /// <summary>
                /// Retrives the raw pointer to the object within the storage.                
                /// </summary>
                /// <typeparam name="T">Pointer type to cast to.</typeparam>
                /// <returns>The raw pointer.</returns>
                template<std::derived_from<RHIObject> T> T* GetObjectPtr(Handle handle) const {
                    return static_cast<T*>(m_objects.at(handle).get());
                }
                /// <summary>
                /// Destorys the object associated with the given handle, and frees the handle for reuse.
                /// </summary>
                /// <param name="handle"></param>
                inline void DestoryObject(Handle handle) {
                    m_objects.erase(handle);
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
