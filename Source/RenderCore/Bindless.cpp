#include "Bindless.hpp"
#include <RHICore/Device.hpp>
#include <algorithm>
#include <mutex>

namespace Foundation::RenderCore
{
    uint32_t BindlessPool::UpdateDescriptor(uint32_t id, RHITextureView* view)
    {
        mDescSet->Update({.binding = 0,
                          .startIndex = id,
                          .type = RHIDescriptorType::SampledImage,
                          .images = {{{.imageView = view, .layout = RHITextureLayout::ShaderReadOnly}}}});
        return id;
    }
    void BindlessPool::UpdateStorageDescriptor(uint32_t id, RHITextureView* view)
    {
        CHECK(view);
        mStorageDescSet->Update({.binding = 0,
                                 .startIndex = id,
                                 .type = RHIDescriptorType::StorageImage,
                                 .images = {{{.imageView = view, .layout = RHITextureLayout::General}}}});
    }
    BindlessPool::BindlessPool(RHIDevice* device, Allocator* allocator, BindlessPoolDesc const& desc) :
        mDevice(device), mAllocator(allocator), mBindings(allocator), mFreeBindings(allocator),
        mIdleGuard(device), mDesc(desc)
    {
        CHECK(desc.maxBindings != 0);
        mBindings.reserve(desc.maxBindings);
        CreateDescriptors(desc.maxBindings);
    }
    void BindlessPool::CreateDescriptors(uint32_t capacity)
    {
        // Sets reference their pools/layouts; release them first when growing.
        mDescSet.Reset();
        mStorageDescSet.Reset();
        mDescPool.Reset();
        mStorageDescPool.Reset();
        mDescLayout.Reset();
        mStorageDescLayout.Reset();
        mDescPool = mDevice->CreateDescriptorPool(
            {.bindings = {{{.type = RHIDescriptorType::SampledImage, .maxCount = capacity}}},
             .updateAfterBind = true});
        mDescLayout = mDevice->CreateDescriptorSetLayout({.bindings = {{{.count = capacity,
                                                                         .stage = RHIShaderStageBits::All,
                                                                         .type = RHIDescriptorType::SampledImage}}},
                                                          .updateAfterBind = true});
        mDescSet = mDescPool->CreateDescriptorSet(mDescLayout, capacity);
        mStorageDescPool = mDevice->CreateDescriptorPool(
            {.bindings = {{{.type = RHIDescriptorType::StorageImage, .maxCount = capacity}}}});
        mStorageDescLayout = mDevice->CreateDescriptorSetLayout(
            {.bindings = {{{.count = capacity,
                            .stage = RHIShaderStageBits::All,
                            .type = RHIDescriptorType::StorageImage}}}});
        mStorageDescSet = mStorageDescPool->CreateDescriptorSet(mStorageDescLayout, capacity);
    }
    bool BindlessPool::Reserve(uint32_t minimumCapacity)
    {
        std::unique_lock lock(mDescMutex);
        if (minimumCapacity <= mDesc.maxBindings)
            return false;
        uint64_t const doubled = static_cast<uint64_t>(mDesc.maxBindings) * 2u;
        uint32_t const capacity = std::max(minimumCapacity,
                                           doubled <= UINT32_MAX ? static_cast<uint32_t>(doubled) : minimumCapacity);
        mIdleGuard.WaitIdle();
        mBindings.reserve(capacity);
        CreateDescriptors(capacity);
        for (auto const& slot : mBindings)
        {
            if (!slot)
                continue;
            Binding const& binding = *slot;
            RHITextureView* view = binding.view.Visit(
                [](RHITextureView* value) { return value; },
                [](RHITextureScopedHandle<RHITextureView> const& value) { return value.Get(); });
            UpdateDescriptor(binding.id, view);
            if (binding.storageView)
                UpdateStorageDescriptor(binding.id, binding.storageView);
        }
        mDesc.maxBindings = capacity;
        return true;
    }
    BindlessPool::Binding* BindlessPool::FindBinding(uint32_t id)
    {
        return id < mBindings.size() ? mBindings[id].GetIf() : nullptr;
    }
    uint32_t BindlessPool::AllocateBinding(Binding&& binding)
    {
        uint32_t id;
        if (mFreeBindings.empty())
        {
            CHECK_MSG(mBindings.size() < mDesc.maxBindings,
                      "Bindless capacity {} exhausted; reserve descriptors before recording", mDesc.maxBindings);
            id = static_cast<uint32_t>(mBindings.size());
            mBindings.emplace_back();
        }
        else
        {
            id = mFreeBindings.back();
            mFreeBindings.pop_back();
        }
        binding.id = id;
        AddStats(mBindings[id].emplace(std::move(binding)));
        return id;
    }
    void BindlessPool::AddStats(Binding const& binding)
    {
        mActiveBindings++;
        mReferencedTextureBytes += binding.referencedTextureBytes;
        if (binding.resource.GetIf<RHIDeviceScopedHandle<RHITexture>>())
        {
            mOwnedTextureBindings++;
            mOwnedTextureBytes += binding.ownedTextureBytes;
        }
    }
    void BindlessPool::RemoveStats(Binding const& binding)
    {
        mActiveBindings--;
        mReferencedTextureBytes -= binding.referencedTextureBytes;
        if (binding.resource.GetIf<RHIDeviceScopedHandle<RHITexture>>())
        {
            mOwnedTextureBindings--;
            mOwnedTextureBytes -= binding.ownedTextureBytes;
        }
    }
    uint32_t BindlessPool::Allocate(RHITextureView* view)
    {
        std::unique_lock lock(mDescMutex);
        RHITexture* texture = view ? view->GetTexture() : nullptr;
        size_t referencedTextureBytes = texture ? texture->GetAllocationSize() : 0;
        uint32_t const id = AllocateBinding({0u, {texture}, {view}, referencedTextureBytes, 0});
        return UpdateDescriptor(id, view);
    }
    uint32_t BindlessPool::Allocate(RHIDeviceScopedHandle<RHITexture>&& texture, RHITextureScopedHandle<RHITextureView>&& view)
    {
        std::unique_lock lock(mDescMutex);
        RHITextureView* rawView = view.Get();
        RHITexture* rawTexture = texture.Get();
        size_t ownedTextureBytes = rawTexture ? rawTexture->GetAllocationSize() : 0;
        uint32_t const id = AllocateBinding({0u, {std::move(texture)}, {std::move(view)},
                                             ownedTextureBytes, ownedTextureBytes});
        return UpdateDescriptor(id, rawView);
    }
    uint32_t BindlessPool::Update(uint32_t id, RHIDeviceScopedHandle<RHITexture>&& texture, RHITextureScopedHandle<RHITextureView>&& view)
    {
        std::unique_lock lock(mDescMutex);
        Binding* binding = FindBinding(id);
        CHECK_MSG(binding, "Cannot update invalid bindless texture binding {}", id);
        RHITextureView* rawView = view.Get();
        RHITexture* rawTexture = texture.Get();
        size_t ownedTextureBytes = rawTexture ? rawTexture->GetAllocationSize() : 0;
        mIdleGuard.WaitIdle();
        UpdateDescriptor(id, rawView);
        RemoveStats(*binding);
        mBindings[id].reset();
        AddStats(mBindings[id].emplace(Binding{id, {std::move(texture)}, {std::move(view)}, ownedTextureBytes,
                                              ownedTextureBytes}));
        return id;
    }
    void BindlessPool::UpdateStorageView(uint32_t id, RHITextureView* view)
    {
        std::unique_lock lock(mDescMutex);
        Binding* binding = FindBinding(id);
        CHECK_MSG(binding, "Cannot update invalid storage texture binding {}", id);
        binding->storageView = view;
        // A cleared slot is never accessed by a shader. Populate it again before reuse.
        if (view)
            UpdateStorageDescriptor(id, view);
    }
    void BindlessPool::Free(uint32_t id)
    {
        std::unique_lock lock(mDescMutex);
        if (Binding* binding = FindBinding(id))
        {
            RemoveStats(*binding);
            mBindings[id].reset();
            mFreeBindings.push_back(id);
        }
    }
    RHITexture* BindlessPool::GetResource(uint32_t id)
    {
        std::unique_lock lock(mDescMutex);
        Binding* binding = FindBinding(id);
        if (!binding)
            return nullptr;

        return binding->resource.Visit(
            [](RHITexture* texture) -> RHITexture* { return texture; },
            [](RHIDeviceScopedHandle<RHITexture> const& texture) -> RHITexture* { return texture.Get(); });
    }
    RHITextureView* BindlessPool::GetView(uint32_t id)
    {
        std::unique_lock lock(mDescMutex);
        Binding* binding = FindBinding(id);
        if (!binding)
            return nullptr;

        return binding->view.Visit(
            [](RHITextureView* view) -> RHITextureView* { return view; },
            [](RHITextureScopedHandle<RHITextureView> const& view) -> RHITextureView* { return view.Get(); });
    }
    BindlessPool::Stats BindlessPool::GetStats() const
    {
        std::unique_lock lock(mDescMutex);
        return {
            .activeBindings = mActiveBindings,
            .capacity = mDesc.maxBindings,
            .ownedTextureBindings = mOwnedTextureBindings,
            .referencedTextureBytes = mReferencedTextureBytes,
            .ownedTextureBytes = mOwnedTextureBytes,
        };
    }
} // namespace Foundation::RenderCore
