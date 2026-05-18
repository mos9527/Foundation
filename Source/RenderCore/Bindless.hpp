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
            RHIDeviceScopedHandle<RHITexture>
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
            size_t referencedTextureBytes{};
            size_t ownedTextureBytes{};
        };

        RHIDevice* const mDevice;
        Allocator* const mAllocator;

        AtomicPool<Binding> mBindings;

        RHIDeviceScopedHandle<RHIDeviceDescriptorSetLayout> mDescLayout;
        RHIDeviceScopedHandle<RHIDeviceDescriptorPool> mDescPool;
        RHIDeviceDescriptorPoolScopedHandle<RHIDeviceDescriptorSet> mDescSet;

        Mutex mDescMutex;
        uint32_t mActiveBindings{0};
        uint32_t mOwnedTextureBindings{0};
        size_t mReferencedTextureBytes{0};
        size_t mOwnedTextureBytes{0};

        RHIDeviceIdleGuard mIdleGuard;

        uint32_t UpdateDescriptor(uint32_t id, RHITextureView* view);
        void AddStats(Binding const& binding);
        void RemoveStats(Binding const& binding);
    public:
        struct BindlessPoolDesc
        {
            uint32_t maxBindings = 1024;
        };
        struct Stats
        {
            uint32_t activeBindings{};
            uint32_t capacity{};
            uint32_t ownedTextureBindings{};
            size_t referencedTextureBytes{};
            size_t ownedTextureBytes{};
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
        uint32_t Allocate(RHIDeviceScopedHandle<RHITexture>&& texture, RHITextureScopedHandle<RHITextureView>&& view);
        /**
         * @brief Replaces an owned texture binding in-place while keeping the same bindless id.
         */
        uint32_t Update(uint32_t id, RHIDeviceScopedHandle<RHITexture>&& texture, RHITextureScopedHandle<RHITextureView>&& view);
        /**
         * @brief Free a binding, and - with it - the owned resource (if any).
         */
        void Free(uint32_t id);

        RHITexture* GetResource(uint32_t id);
        RHITextureView* GetView(uint32_t id);
        [[nodiscard]] Stats GetStats() const;

        RHIDeviceDescriptorSetLayout* GetDescriptorSetLayout() const { return mDescLayout.Get(); }
        RHIDeviceDescriptorSet* GetDescriptorSet() const { return mDescSet.Get(); }
    };
}