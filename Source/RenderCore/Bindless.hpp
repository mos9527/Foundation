#pragma once
#include <RHICore/Resource.hpp>
#include <RHICore/Command.hpp>
#include <Core/Variant.hpp>
#include <Core/AtomicPool.hpp>
namespace Foundation::RenderCore
{
    using namespace RHI;
    // TODO: Buffers?
    class BindlessPool
    {
        using Resource = Variant<
            RHITexture*,
            RHIDeviceScopedObjectHandle<RHITexture>
        >;
        using View = Variant<
            // Texture Views
            RHITextureView*,
            RHITextureScopedHandle<RHITextureView>
        >;
        struct Binding
        {
            uint32_t id;
            Resource resource;
            View view;
        };

        RHIDevice* const mDevice;
        Allocator* const mAllocator;

        AtomicPool<Binding> mBindings;

        RHIDeviceScopedObjectHandle<RHIDeviceDescriptorSetLayout> mDescLayout;
        RHIDeviceScopedObjectHandle<RHIDeviceDescriptorPool> mDescPool;
        RHIDeviceDescriptorPoolScopedHandle<RHIDeviceDescriptorSet> mDescSet;

        Mutex mDescMutex;

        RHIDeviceIdleGuard mIdleGuard;
    public:
        struct BindlessPoolDesc
        {
            uint32_t maxBindings = 1024;
        };
        const BindlessPoolDesc mDesc;
        BindlessPool(RHIDevice* device, Allocator * allocator, BindlessPoolDesc const& desc);

        uint32_t Allocate(RHITextureView* view);
        void Free(uint32_t id);

        RHIDeviceDescriptorSetLayout* GetDescriptorSetLayout() const { return mDescLayout.Get(); }
        RHIDeviceDescriptorSet* GetDescriptorSet() const { return mDescSet.Get(); }
    };
}