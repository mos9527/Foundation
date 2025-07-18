#pragma once
#include <Platform/RHI/Common.hpp>
namespace Foundation {
    namespace Platform {
        namespace RHI {
            class RHIBuffer;
            class RHIDeviceDescriptorPool;
            class RHIDeviceDescriptorSetLayout;
            template<typename T> using RHIDeviceDescriptorPoolHandle = RHIHandle<RHIDeviceDescriptorPool, T>;
            template<typename T> using RHIDeviceDescriptorPoolScopedHandle = RHIScopedHandle<RHIDeviceDescriptorPool, T>;
            class RHIDeviceDescriptorSet : public RHIObject {
            protected:
                const RHIDeviceDescriptorPool& m_pool;
            public:
                RHIDeviceDescriptorSet(RHIDeviceDescriptorPool const& pool) : m_pool(pool) {}
                struct UpdateDesc {                    
                    struct Buffer {
                        RHIBuffer* buffer{ nullptr }; // Buffer to bind
                        size_t offset{ 0 }; // Offset in bytes
                        size_t size{ kFullSize }; // Size in bytes
                    };
                    size_t binding{ 0 }; // 0-indexed, first to update in the descriptor set
                    // Type of descriptor to update
                    // This also determines which of the next spans is used
                    // to update the descriptors.
                    // Implementations should guarantee that descriptor type updates within
                    // a single call is homogenous.
                    RHIDescriptorType type{ RHIDescriptorType::UniformBuffer };
                    Core::StlSpan<const Buffer> buffers; // Buffers to update starting from `binding`
                };
                virtual void Update(UpdateDesc const& desc) = 0;
            };
            class RHIDeviceDescriptorPool : public RHIObject {
            protected:
                const RHIDevice& m_device;
            public:
                struct PoolDesc {
                    struct Binding {
                        RHIDescriptorType type; // Type of this binding
                        uint32_t max_count{ 1 }; // Max number of descriptors of this type that can be allocated
                    };
                    Core::StlSpan<const Binding> bindings;
                } const m_desc;
                RHIDeviceDescriptorPool(RHIDevice const& device, PoolDesc const& desc)
                    : m_device(device), m_desc(desc) {
                }
                virtual RHIDeviceDescriptorPoolScopedHandle<RHIDeviceDescriptorSet> CreateDescriptorSet(
                    RHIDeviceObjectHandle<RHIDeviceDescriptorSetLayout>) = 0;
                virtual RHIDeviceDescriptorSet* GetDescriptorSet(Handle handle) const = 0;
                virtual void DestroyDescriptorSet(Handle handle) = 0;
            };
            template<> struct RHIObjectTraits<RHIDeviceDescriptorSet> {
                static RHIDeviceDescriptorSet* Get(RHIDeviceDescriptorPool const* pool, Handle handle) {
                    return pool->GetDescriptorSet(handle);
                }
                static void Destroy(RHIDeviceDescriptorPool* pool, Handle handle) {
                    pool->DestroyDescriptorSet(handle);
                }
            };
        }
    }
}
