#pragma once
#include "Common.hpp"
namespace Foundation::RHI {
    class RHIBuffer;
    class RHITextureView;
    class RHIDeviceSampler;
    class RHIDeviceDescriptorPool;
    class RHIDeviceDescriptorSetLayout;
    class RHIAccelerationStructure;
    template<typename T> using RHIDeviceDescriptorPoolHandle = RHIHandle<RHIDeviceDescriptorPool, T>;
    template<typename T> using RHIDeviceDescriptorPoolScopedHandle = RHIScopedHandle<RHIDeviceDescriptorPool, T>;
    class RHIDeviceDescriptorSet : public RHIObject {
    protected:
        const RHIDeviceDescriptorPool& mPool;
    public:
        RHIDeviceDescriptorSet(RHIDeviceDescriptorPool const& pool) : mPool(pool) {}
        struct UpdateDesc {
            size_t binding{ 0 }; // 0-indexed, first to update in the descriptor set
            size_t startIndex{ 0 }; // First index in the binding array to update
            RHIDescriptorType type{ RHIDescriptorType::UniformBuffer };
            struct Buffer {
                RHIBuffer* buffer{ nullptr }; // Buffer to bind
                size_t offset{ 0 }; // Offset in bytes
                size_t size{ kFullSize }; // Size in bytes
            };
            Span<const Buffer> buffers; // Applies to type of UniformBuffer, StorageBuffer
            struct Image {
                RHITextureView* imageView{ nullptr }; // Image view to bind, can be null
                RHIDeviceSampler* sampler{ nullptr }; // Sampler to bind, can be null
                RHITextureLayout layout{}; // Layout of the image
            };
            Span<const Image> images; // Applies to type of Sampler, SampledImage
            struct AccelerationStructure
            {
                RHIAccelerationStructure* as{ nullptr }; // Acceleration structure to bind
            };
            Span<const AccelerationStructure> accelerationStructures; // Applies to type of AccelerationStructure
        };
        // NOTE: `desc.type` is used to determine which of the next spans is used                
        // to update the descriptors.
        // Implementations should guarantee that descriptor type updates within
        // a single call is homogenous, and throw if type mismatches the spans given,
        // or some spans are unused.
        virtual void Update(UpdateDesc const& desc) = 0;

        virtual void DebugSetObjectName(const char* name) = 0;
    };
    // XXX: Vulkanism. Not really a thing in other APIs.
    class RHIDeviceDescriptorPool : public RHIObject {
    protected:
        const RHIDevice& mDevice;
    public:
        struct PoolDesc {
            struct Binding {
                RHIDescriptorType type; // Type of this binding
                uint32_t maxCount{ 1 }; // Max number of descriptors of this type that can be allocated
            };
            // Bindings that make up this pool
            Span<const Binding> bindings;
            // Allow updating descriptors after being bound to a command buffer when
            // they are not used
            bool updateAfterBind{ false };
        } const mDesc;
        RHIDeviceDescriptorPool(RHIDevice const& device, PoolDesc const& desc)
            : mDevice(device), mDesc(desc) {
        }
        [[nodiscard]] virtual RHIDeviceDescriptorPoolScopedHandle<RHIDeviceDescriptorSet> CreateDescriptorSet(
            RHIDeviceHandle<RHIDeviceDescriptorSetLayout>, uint32_t max_variable_count = 0) = 0;
        [[nodiscard]] virtual RHIDeviceDescriptorSet* GetDescriptorSet(Handle handle) const = 0;
        virtual void DestroyDescriptorSet(Handle handle) = 0;

        virtual void DebugSetObjectName(const char* name) = 0;
    };
    template<> struct RHIObjectTraits<RHIDeviceDescriptorPool, RHIDeviceDescriptorSet> {
        static RHIDeviceDescriptorSet* Get(RHIDeviceDescriptorPool const* pool, Handle handle) {
            return pool->GetDescriptorSet(handle);
        }
        static void Destroy(RHIDeviceDescriptorPool* pool, Handle handle) {
            pool->DestroyDescriptorSet(handle);
        }
    };
}
