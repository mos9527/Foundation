#pragma once
#include <Core/Allocator/Allocator.hpp>
#include <Core/Container/FreeList.hpp>
#include <RHICore/Resource.hpp>
namespace Foundation::Renderer {
    /// <summary>
    /// Reference counted facilities for Render Graph resources.
    /// </summary>
    using Handle = uint64_t;
    using RefCount = uint64_t;
    constexpr static Handle kInvalidHandle = static_cast<Handle>(-1);
    template<typename T> class RGResourceStorage;
    // IMPORTANT: It's UB for the handle to go out of scope *BEFORE* the factory/storage does   
    template<typename T> struct RGResourceHandle {
        RGResourceStorage<T>* m_factory{ nullptr };
        Handle m_handle{ kInvalidHandle };
        inline void Invalidate() {
            m_factory = nullptr;
            m_handle = kInvalidHandle;
        }
        void RefIncrement() { if (IsValid()) m_factory->RefIncrement(m_handle); }
        void RefDecrement() { if (IsValid()) m_factory->RefDecrement(m_handle); }
    public:
        RGResourceHandle(RGResourceStorage<T>* factory, Handle handle) : m_factory(factory), m_handle(handle) {
            RefIncrement();
        }
        RGResourceHandle(RGResourceHandle const& other) : m_factory(other.m_factory), m_handle(other.m_handle) {
            RefIncrement();
        }
        RGResourceHandle(RGResourceHandle&& other) noexcept : m_factory(other.m_factory), m_handle(other.m_handle) {
            other.Invalidate();
        }
        RGResourceHandle& operator=(RGResourceHandle const& other) {
            if (this != &other) {
                RefDecrement();
                m_factory = other.m_factory;
                m_handle = other.m_handle;
                RefIncrement();
            }
            return *this;
        }
        RGResourceHandle& operator=(RGResourceHandle&& other) noexcept {
            if (this != &other) {
                RefDecrement();
                m_factory = other.m_factory;
                m_handle = other.m_handle;
                other.Invalidate();
            }
            return *this;
        }
        ~RGResourceHandle() { RefDecrement(); }
        const RefCount GetRefCount() const { return m_factory->RefGetCount(m_handle); }
        constexpr const bool IsValid() const noexcept { return m_factory && m_handle != kInvalidHandle; }
        constexpr const bool operator()() const noexcept { return IsValid(); }
        const T& GetResource() const { return m_factory->GetResource(m_handle); }
        T& GetResource() { return m_factory->GetResource(m_handle); }
    };
    // Refcounted storage for Render Graph resources.
    // Quite similar to RHIObjectStorage, but with ref-counting and in-place construction
    // since with RHIObjects - we guaranteed that objects do not even *move*.
    // Thus any dense storage that involves moving memory when resizing is not suitable
    // hence the use of UniquePtr in RHIObjectStorage.
    // Here we present one that *requires* movable types, and allows in-place construction
    // with tracking of lifetime since many resources are
    // - Transient (e.g. Framebuffers, Render Passes, etc.)
    // - Aliased (e.g. Images, Buffers, that can be reused from some form of allocator/GC)
    // - Again. Render passes don't own *anything* even if they 'created' the resources.
    template<typename T>
    class RGResourceStorage {
        Core::Allocator* m_allocator;
        Core::FreeDenseMap<Handle, std::pair<RefCount, T>> m_objects;        
    public:
        RGResourceStorage(Core::Allocator* alloc) : m_allocator(alloc), m_objects(alloc) {}
        /// <summary>
        /// Constructs a new resource in-place and returns a ref-counted handle to it.
        /// </summary>        
        template<typename ...Args> RGResourceHandle<T> Emplace(Args&&... args) {            
            auto [handle, value] = m_objects.allocate();
            std::construct_at(&value.second, std::forward<Args>(args)...);
            return { this, handle };
        }
        RefCount RefGetCount(Handle handle) const { return m_objects.at(handle).first; }
        void RefIncrement(Handle handle) { m_objects.at(handle).first++; }
        void RefDecrement(Handle handle) {
            if (m_objects.at(handle).first)
                m_objects.at(handle).first--;
            else
                m_objects.free(handle);
        }
        T const & GetResource(Handle handle) const { return m_objects.at(handle).second; }
        T& GetResource(Handle handle) { return m_objects.at(handle).second; }
    };
}
