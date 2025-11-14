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

        uint32_t Update(uint32_t id, RHITextureView* view);
    public:
        struct BindlessPoolDesc
        {
            uint32_t maxBindings = 1024;
        };
        const BindlessPoolDesc mDesc;
        BindlessPool(RHIDevice* device, Allocator * allocator, BindlessPoolDesc const& desc);

        /**
         * @breif Create a binding for the given texture view, with ownership remaining with the caller.
         */
        uint32_t Allocate(RHITextureView* view);
        /**
         * @brief Takes ownership of the texture, and creates a binding for it.
         */
        uint32_t Allocate(RHIDeviceScopedObjectHandle<RHITexture>&& texture, RHITextureView* view);
        /**
         * @brief Free a binding, and - with it - the owned resource (if any).
         */
        void Free(uint32_t id);

        Resource& GetResource(uint32_t id) { return mBindings.At(id)->resource; }
        View& GetView(uint32_t id) { return mBindings.At(id)->view; }

        RHIDeviceDescriptorSetLayout* GetDescriptorSetLayout() const { return mDescLayout.Get(); }
        RHIDeviceDescriptorSet* GetDescriptorSet() const { return mDescSet.Get(); }
    };
}