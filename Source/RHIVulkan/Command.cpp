using namespace Foundation::RHI;
VulkanCommandPool::VulkanCommandPool(const VulkanDevice& device, PoolDesc const& desc, Allocator* allocator) :
    RHICommandPool(device, desc), mAllocator(allocator), mDevice(device), mStorage(allocator)
{
    // Create the command pool
    vk::CommandPoolCreateFlagBits flag{};
    switch (desc.type)
    {
    case RHICommandPoolType::Transient:
        flag = vk::CommandPoolCreateFlagBits::eTransient;
        break;
    default:
    case RHICommandPoolType::Persistent:
        flag = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
        break;
    }
    vk::CommandPoolCreateInfo pool_info{
        .flags = flag,
        .queueFamilyIndex = static_cast<VulkanDeviceQueue*>(desc.queue)->GetVkQueueFamily(),
    };
    mCommandPool = vk::raii::CommandPool(device.GetVkDevice(), pool_info, mDevice.GetVkAllocatorCallbacks());
    CHECK(mCommandPool != nullptr && "failed to create Vulkan command pool");
}

void VulkanCommandPool::DebugSetObjectName(const char* name)
{
    VkCommandPool handle = *mCommandPool;
    mDevice.GetVkDevice().setDebugUtilsObjectNameEXT({.objectType = vk::ObjectType::eCommandPool,
                                                      .objectHandle = reinterpret_cast<uint64_t>(handle),
                                                      .pObjectName = name});
}

RHICommandPoolScopedHandle<RHICommandList> VulkanCommandPool::CreateCommandList()
{
    return {this, mStorage.CreateObject<VulkanCommandList>(*this)};
}
RHICommandList* VulkanCommandPool::GetCommandList(Handle handle) const
{
    return mStorage.GetObjectPtr<RHICommandList>(handle);
}
void VulkanCommandPool::DestroyCommandList(Handle handle) { mStorage.DestroyObject(handle); }

void VulkanCommandPool::ResetAllCommandLists(bool freeResources)
{
    mCommandPool.reset(freeResources ? vk::CommandPoolResetFlagBits::eReleaseResources : vk::CommandPoolResetFlags{});
}

VulkanCommandList::VulkanCommandList(const VulkanCommandPool& commandPool) :
    RHICommandList(commandPool), mCommandPool(commandPool)
{
    vk::CommandBufferAllocateInfo alloc_info{
        .commandPool = *mCommandPool.GetVkCommandPool(),
        .level = vk::CommandBufferLevel::ePrimary,
        .commandBufferCount = 1,
    };
    mCommandBuffer = std::move(vk::raii::CommandBuffers(mCommandPool.GetDevice().GetVkDevice(), alloc_info).front());
    CHECK(mCommandBuffer != nullptr && "failed to allocate Vulkan command buffer");
}

RHICommandList& VulkanCommandList::Begin()
{
    bool isTransient = mCommandPool.mDesc.type == RHICommandPoolType::Transient;
    vk::CommandBufferBeginInfo beginInfo{.flags = isTransient ? vk::CommandBufferUsageFlagBits::eOneTimeSubmit
                                                              : vk::CommandBufferUsageFlags{}};
    mCommandBuffer.begin(beginInfo);
    mAllocator = mCommandPool.GetDevice().GetAllocator();
    return *this;
}
RHICommandList& VulkanCommandList::BeginTransition()
{
    CHECK(mAllocator && "Invalid command list states.");
    mBarriers = ConstructUnique<Barriers>(mAllocator, mAllocator);
    return *this;
}
#include "Resource.hpp"
RHICommandList& VulkanCommandList::SetBufferTransition(RHIBuffer* image, TransitionDesc const& desc)
{
    CHECK(mBarriers && "Invalid barrier states.");
    auto* res = static_cast<VulkanBuffer*>(image);
    mBarriers->buffer.push_back(
        vk::BufferMemoryBarrier2{.srcStageMask = vkPipelineStageFlags2FromRHIPipelineStage(desc.srcStage),
                                 .srcAccessMask = vkAccessFlagsFromRHIResourceAccess(desc.srcAccess),
                                 .dstStageMask = vkPipelineStageFlags2FromRHIPipelineStage(desc.dstStage),
                                 .dstAccessMask = vkAccessFlagsFromRHIResourceAccess(desc.dstAccess),
                                 .srcQueueFamilyIndex = desc.srcQueueIndex,
                                 .dstQueueFamilyIndex = desc.dstQueueIndex,
                                 .buffer = *res->GetVkBuffer(),
                                 .offset = desc.srcBufferOffset,
                                 .size = desc.srcBufferSize == kFullSize ? VK_WHOLE_SIZE : desc.srcBufferSize});
    return *this;
}
RHICommandList& VulkanCommandList::SetImageTransition(RHITexture* image, TransitionDesc const& desc)
{
    CHECK(mBarriers && "Invalid barrier states.");
    CHECK_MSG(desc.srcImgRange.layer.aspect.value, "Aspect flag on transition subresource is NULL!!");
    auto* res = static_cast<VulkanTexture*>(image);
    mBarriers->image.push_back(vk::ImageMemoryBarrier2{
        .srcStageMask = vkPipelineStageFlags2FromRHIPipelineStage(desc.srcStage),
        .srcAccessMask = vkAccessFlagsFromRHIResourceAccess(desc.srcAccess),
        .dstStageMask = vkPipelineStageFlags2FromRHIPipelineStage(desc.dstStage),
        .dstAccessMask = vkAccessFlagsFromRHIResourceAccess(desc.dstAccess),
        .oldLayout = vkImageLayoutFromRHITextureLayout(desc.srcImgLayout),
        .newLayout = vkImageLayoutFromRHITextureLayout(desc.dstImgLayout),
        .srcQueueFamilyIndex = desc.srcQueueIndex,
        .dstQueueFamilyIndex = desc.dstQueueIndex,
        .image = *res->GetVkImage(),
        .subresourceRange = {.aspectMask = vkImageAspectFlagFromRHITextureAspect(desc.srcImgRange.layer.aspect),
                             .baseMipLevel = desc.srcImgRange.layer.mipLevel,
                             .levelCount = desc.srcImgRange.mipCount,
                             .baseArrayLayer = desc.srcImgRange.layer.baseArrayLayer,
                             .layerCount = desc.srcImgRange.layer.layerCount}});
    return *this;
}
RHICommandList& VulkanCommandList::EndTransition()
{
    CHECK(mBarriers && "Invalid barrier states.");
    if (mBarriers->image.empty() && mBarriers->buffer.empty())
        return *this; // No transitions to apply
    vk::DependencyInfo dependency_info{
        .bufferMemoryBarrierCount = static_cast<uint32_t>(mBarriers->buffer.size()),
        .pBufferMemoryBarriers = mBarriers->buffer.data(),
        .imageMemoryBarrierCount = static_cast<uint32_t>(mBarriers->image.size()),
        .pImageMemoryBarriers = mBarriers->image.data(),
    };
    mCommandBuffer.pipelineBarrier2(dependency_info);
    mBarriers.reset();
    return *this;
}

#include "PipelineState.hpp"
RHICommandList& VulkanCommandList::SetPipeline(PipelineDesc const& desc)
{
    CHECK(mAllocator && "Invalid command list states.");
    CHECK(desc.pipeline && "Pipeline is invalid.");
    mCommandBuffer.bindPipeline(vkPipelineBindPointFromRHIDevicePipelineType(desc.type),
                                *static_cast<VulkanPipelineState*>(desc.pipeline)->GetVkPipeline());
    return *this;
}

RHICommandList& VulkanCommandList::SetViewport(float x, float y, float width, float height, float depth_min,
                                               float depth_max, bool flipY)
{
    CHECK(mAllocator && "Invalid command list states.");
    if (flipY)
    {
        y = height - y;
        height = -height;
    }
    vk::Viewport viewport{x, y, width, height, depth_min, depth_max};
    mCommandBuffer.setViewport(0, viewport);
    return *this;
}

RHICommandList& VulkanCommandList::SetScissor(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    CHECK(mAllocator && "Invalid command list states.");
    vk::Rect2D scissor{{static_cast<int32_t>(x), static_cast<int32_t>(y)}, {width, height}};
    mCommandBuffer.setScissor(0, scissor);
    return *this;
}

RHICommandList& VulkanCommandList::Draw(uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex,
                                        uint32_t first_instance)
{
    CHECK(mAllocator && "Invalid command list states.");
    mCommandBuffer.draw(vertex_count, instance_count, first_vertex, first_instance);
    return *this;
}

RHICommandList& VulkanCommandList::DrawIndexed(uint32_t index_count, uint32_t instance_count, uint32_t first_index,
                                               int32_t vertex_offset, uint32_t first_instance)
{
    CHECK(mAllocator && "Invalid command list states.");
    mCommandBuffer.drawIndexed(index_count, instance_count, first_index, vertex_offset, first_instance);
    return *this;
}

RHICommandList& VulkanCommandList::DrawIndexedIndirectCount(RHIBuffer* buffer, size_t offset, RHIBuffer* count_buffer,
                                                            size_t count_offset, uint32_t max_draw_count,
                                                            uint32_t stride)
{
    CHECK(mAllocator && "Invalid command list states.");
    mCommandBuffer.drawIndexedIndirectCount(static_cast<VulkanBuffer*>(buffer)->GetVkBuffer(), offset,
                                            static_cast<VulkanBuffer*>(count_buffer)->GetVkBuffer(), count_offset,
                                            max_draw_count, stride);
    return *this;
}

RHICommandList& VulkanCommandList::DrawMeshTasks(uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z)
{
    CHECK(mAllocator && "Invalid command list states.");
    mCommandBuffer.drawMeshTasksEXT(group_count_x, group_count_y, group_count_z);
    return *this;
}

RHICommandList& VulkanCommandList::DrawMeshTasksIndirect(RHIBuffer* cmd_buffer, size_t cmd_offset, size_t draw_count,
                                                         size_t stride)
{
    CHECK(mAllocator && "Invalid command list states.");
    mCommandBuffer.drawMeshTasksIndirectEXT(static_cast<VulkanBuffer*>(cmd_buffer)->GetVkBuffer(), cmd_offset,
                                            draw_count, stride);
    return *this;
}

RHICommandList& VulkanCommandList::PushConstant(RHIPipelineState* pipeline, RHIShaderStage stage, uint32_t offset,
                                                Span<const char> data)
{
    CHECK(mAllocator && "Invalid command list states.");
    vkCmdPushConstants(*mCommandBuffer, *static_cast<VulkanPipelineState*>(pipeline)->GetVkPipelineLayout(),
                       static_cast<VkShaderStageFlags>(vkShaderStageFlagsFromRHIShaderStage(stage)), offset,
                       data.size(), data.data());
    return *this;
}
RHICommandList& VulkanCommandList::UpdateBuffer(RHIBuffer* buffer, size_t offset, Span<const char> data)
{
    CHECK(mAllocator && "Invalid command list states.");
    auto* vulkan_buffer = static_cast<VulkanBuffer*>(buffer);
    mCommandBuffer.updateBuffer<const char>(vulkan_buffer->GetVkBuffer(), offset, data);
    return *this;
}
RHICommandList& VulkanCommandList::FillBuffer(RHIBuffer* buffer, uint32_t value, size_t offset, size_t size)
{
    CHECK(mAllocator && "Invalid command list states.");
    auto* vulkan_buffer = static_cast<VulkanBuffer*>(buffer);
    mCommandBuffer.fillBuffer(*vulkan_buffer->GetVkBuffer(), offset, size == kFullSize ? VK_WHOLE_SIZE : size, value);
    return *this;
}

RHICommandList& VulkanCommandList::CopyBuffer(RHIBuffer* src_buffer, RHIBuffer* dst_buffer,
                                              Span<const CopyBufferRegion> regions)
{
    CHECK(mAllocator && "Invalid command list states.");
    CHECK(src_buffer && dst_buffer && "Source and destination buffers must be valid.");

    auto* src_vulkan_buffer = static_cast<VulkanBuffer*>(src_buffer);
    auto* dst_vulkan_buffer = static_cast<VulkanBuffer*>(dst_buffer);

    Vector<vk::BufferCopy> vk_regions(mAllocator);
    for (auto const& region : regions)
    {
        size_t size = region.size;
        if (size == kFullSize)
        {
            size = std::min(src_vulkan_buffer->mDesc.size - region.srcOffset,
                            dst_vulkan_buffer->mDesc.size - region.dstOffset);
        }
        vk_regions.push_back(vk::BufferCopy{.srcOffset = static_cast<vk::DeviceSize>(region.srcOffset),
                                            .dstOffset = static_cast<vk::DeviceSize>(region.dstOffset),
                                            .size = size});
    }
    mCommandBuffer.copyBuffer(*src_vulkan_buffer->GetVkBuffer(), *dst_vulkan_buffer->GetVkBuffer(), vk_regions);
    return *this;
}

RHICommandList& VulkanCommandList::CopyImage(RHITexture* src_image, RHITextureLayout src_layout, RHITexture* dst_image,
                                             RHITextureLayout dst_layout, Span<const CopyImageRegion> regions)
{
    CHECK(mAllocator && "Invalid command list states.");
    CHECK(src_image && dst_image && "Source and destination images must be valid.");

    auto* src_vulkan_image = static_cast<VulkanTexture*>(src_image);
    auto* dst_vulkan_image = static_cast<VulkanTexture*>(dst_image);

    Vector<vk::ImageCopy> vk_regions(mAllocator);
    vk_regions.reserve(regions.size());
    for (auto const& region : regions)
    {
        vk_regions.push_back(vk::ImageCopy{
            .srcSubresource = {.aspectMask = vkImageAspectFlagFromRHITextureAspect(region.srcLayer.aspect),
                               .mipLevel = region.srcLayer.mipLevel,
                               .baseArrayLayer = region.srcLayer.baseArrayLayer,
                               .layerCount = region.srcLayer.layerCount},
            .srcOffset = {region.srcOffset.x, region.srcOffset.y, region.srcOffset.z},
            .dstSubresource = {.aspectMask = vkImageAspectFlagFromRHITextureAspect(region.dstLayer.aspect),
                               .mipLevel = region.dstLayer.mipLevel,
                               .baseArrayLayer = region.dstLayer.baseArrayLayer,
                               .layerCount = region.dstLayer.layerCount},
            .dstOffset = {region.dstOffset.x, region.dstOffset.y, region.dstOffset.z},
            .extent = {region.extent.x, region.extent.y, region.extent.z}});
    }

    mCommandBuffer.copyImage(*src_vulkan_image->GetVkImage(), vkImageLayoutFromRHITextureLayout(src_layout),
                             *dst_vulkan_image->GetVkImage(), vkImageLayoutFromRHITextureLayout(dst_layout),
                             vk_regions);

    return *this;
}

RHICommandList& VulkanCommandList::CopyBufferToImage(RHIBuffer* src_buffer, RHITexture* dst_image,
                                                     RHITextureLayout dst_layout, Span<const CopyImageRegion> regions)
{
    CHECK(mAllocator && "Invalid command list states.");
    CHECK(src_buffer && dst_image && "Source buffer and destination image must be valid.");

    auto* src_vulkan_buffer = static_cast<VulkanBuffer*>(src_buffer);
    auto* dst_vulkan_image = static_cast<VulkanTexture*>(dst_image);

    Vector<vk::BufferImageCopy> vk_regions(mAllocator);
    vk_regions.reserve(regions.size());
    for (auto const& region : regions)
    {
        vk_regions.push_back(vk::BufferImageCopy{
            .bufferOffset = region.srcBufferOffset,
            .bufferRowLength = 0, // Tightly packed
            .bufferImageHeight = 0, // Tightly packed
            .imageSubresource = {.aspectMask = vkImageAspectFlagFromRHITextureAspect(region.dstLayer.aspect),
                                 .mipLevel = region.dstLayer.mipLevel,
                                 .baseArrayLayer = region.dstLayer.baseArrayLayer,
                                 .layerCount = region.dstLayer.layerCount},
            .imageOffset = {region.dstOffset.x, region.dstOffset.y, region.dstOffset.z},
            .imageExtent = {region.extent.x, region.extent.y, region.extent.z}});
    }

    mCommandBuffer.copyBufferToImage(*src_vulkan_buffer->GetVkBuffer(), *dst_vulkan_image->GetVkImage(),
                                     vkImageLayoutFromRHITextureLayout(dst_layout), vk_regions);

    return *this;
}

RHICommandList& VulkanCommandList::BeginGraphics(GraphicsDesc const& desc)
{
    CHECK(mAllocator && "Invalid command list states.");
    auto vkRenderingAttachmentInfoFromAttachment =
        [](const GraphicsDesc::Attachment& attachment) -> vk::RenderingAttachmentInfo
    {
        vk::ClearValue clear_value{.color = vk::ClearColorValue{}};
        if (attachment.clearColor)
        {
            clear_value.color = vk::ClearColorValue{std::array{attachment.clearColor->x, attachment.clearColor->y,
                                                               attachment.clearColor->z, attachment.clearColor->w}};
        }
        else if (attachment.clearDepthStencil)
        {
            clear_value.depthStencil =
                vk::ClearDepthStencilValue{attachment.clearDepthStencil->first, attachment.clearDepthStencil->second};
        }
        vk::AttachmentLoadOp loadOp = attachment.clearColor || attachment.clearDepthStencil
            ? vk::AttachmentLoadOp::eClear
            : vk::AttachmentLoadOp::eLoad;
        return vk::RenderingAttachmentInfo{
            .imageView = attachment.imageView
                ? static_cast<const VulkanTextureView*>(attachment.imageView)->GetVkImageView()
                : vk::ImageView{nullptr},
            .imageLayout = vkImageLayoutFromRHITextureLayout(attachment.imageLayout),
            .loadOp = loadOp,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue = clear_value};
    };
    Vector<vk::RenderingAttachmentInfo> attachments(mAllocator);
    attachments.reserve(desc.colorAttachments.size());
    for (auto const& attachment : desc.colorAttachments)
    {
        attachments.push_back(vkRenderingAttachmentInfoFromAttachment(attachment));
    }
    vk::RenderingAttachmentInfo depth_attachment = vkRenderingAttachmentInfoFromAttachment(desc.depthAttachment);
    vk::RenderingAttachmentInfo stencil_attachment = vkRenderingAttachmentInfoFromAttachment(desc.stencilAttachment);
    vk::RenderingInfo renderingInfo{
        .renderArea = vk::Rect2D{{0, 0}, vk::Extent2D{desc.width, desc.height}},
        .layerCount = 1,
        .colorAttachmentCount = static_cast<uint32_t>(attachments.size()),
        .pColorAttachments = attachments.data(),
        .pDepthAttachment = desc.depthAttachment.IsValid() ? &depth_attachment : nullptr,
        .pStencilAttachment = desc.stencilAttachment.IsValid() ? &stencil_attachment : nullptr,
    };
    mCommandBuffer.beginRendering(renderingInfo);
    return *this;
}

RHICommandList& VulkanCommandList::BindVertexBuffer(uint32_t index, Span<RHIBuffer* const> buffers,
                                                    Span<const size_t> offsets)
{
    CHECK(mAllocator && "Invalid command list states.");
    CHECK(buffers.size() == offsets.size() && "Buffers and offsets must have the same size.");

    Vector<vk::Buffer> vk_buffers(mAllocator);
    Vector<vk::DeviceSize> vk_offsets(mAllocator);
    for (size_t i = 0; i < buffers.size(); ++i)
    {
        auto* buffer = static_cast<VulkanBuffer*>(buffers[i]);
        vk_buffers.push_back(*buffer->GetVkBuffer());
        vk_offsets.push_back(static_cast<vk::DeviceSize>(offsets[i]));
    }
    mCommandBuffer.bindVertexBuffers(index, vk_buffers, vk_offsets);
    return *this;
}

RHICommandList& VulkanCommandList::BindIndexBuffer(RHIBuffer* buffer, size_t offset, RHIResourceFormat format)
{
    CHECK(mAllocator && "Invalid command list states.");
    CHECK(buffer && "Buffer must be valid.");
    vk::IndexType type;
    switch (format)
    {
    case RHIResourceFormat::R32Uint:
        type = vk::IndexType::eUint32;
        break;
    case RHIResourceFormat::R16Uint:
        type = vk::IndexType::eUint16;
        break;
    default:
        throw std::runtime_error("unsupported index format");
    }
    auto* vulkan_buffer = static_cast<VulkanBuffer*>(buffer);
    mCommandBuffer.bindIndexBuffer(*vulkan_buffer->GetVkBuffer(), static_cast<vk::DeviceSize>(offset), type);
    return *this;
}

#include "Descriptor.hpp"
RHICommandList& VulkanCommandList::BindDescriptorSet(RHIDevicePipelineType bindpoint, RHIPipelineState* pipeline,
                                                     Span<RHIDeviceDescriptorSet* const> sets, size_t first,
                                                     Span<const uint32_t> dynamicOffsets)
{
    CHECK(mAllocator && "Invalid command list states.");
    Vector<vk::DescriptorSet> vk_sets(mAllocator);
    for (auto* set : sets)
    {
        CHECK(set && "Descriptor set must be valid.");
        auto* vulkan_set = static_cast<VulkanDeviceDescriptorSet*>(set);
        vk_sets.push_back(*(vulkan_set->GetVkDescriptorSet()));
    }
    mCommandBuffer.bindDescriptorSets(vkPipelineBindPointFromRHIDevicePipelineType(bindpoint),
                                      static_cast<VulkanPipelineState*>(pipeline)->GetVkPipelineLayout(),
                                      static_cast<uint32_t>(first), vk_sets, dynamicOffsets);
    return *this;
}

RHICommandList& VulkanCommandList::EndGraphics()
{
    CHECK(mAllocator && "Invalid command list states.");
    mCommandBuffer.endRendering();
    return *this;
}

RHICommandList& VulkanCommandList::Dispatch(uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z)
{
    CHECK(mAllocator && "Invalid command list states.");
    CHECK_MSG(group_count_x > 0 && group_count_y > 0 && group_count_z > 0,
              "Dispatch group counts must be greater than zero.");
    mCommandBuffer.dispatch(group_count_x, group_count_y, group_count_z);
    return *this;
}
RHICommandList& VulkanCommandList::BuildAccelerationStructure(Span<const RHIAccelerationStructureBuildDesc> desc)
{
    CHECK(mAllocator && "Invalid command list states.");
    return *this;
}
RHICommandList& VulkanCommandList::WriteTimestamp(RHIDeviceQueryPool* pool, RHIPipelineStageBits stage,
                                                  uint32_t queryIndex)
{
    CHECK(mAllocator && "Invalid command list states.");
    auto* vkPool = static_cast<VulkanDeviceQueryPool*>(pool);
    mCommandBuffer.writeTimestamp(vkFlagsToBits(vkPipelineStageFlagsFromRHIPipelineStage(stage)),
                                  *vkPool->GetVkQueryPool(), queryIndex);
    return *this;
}

void VulkanCommandList::End()
{
    CHECK(mAllocator && "Invalid command list states.");
    mCommandBuffer.end();
    mAllocator = nullptr;
}

void VulkanCommandList::Reset()
{
    mAllocator = nullptr;
    mCommandBuffer.reset();
}

RHICommandList& VulkanCommandList::DebugBegin(const char* message)
{
    CHECK(mAllocator && "Invalid command list states.");
    CHECK(message && "Debug message must not be null");
    mCommandBuffer.beginDebugUtilsLabelEXT({.pLabelName = message});
    return *this;
}
RHICommandList& VulkanCommandList::DebugInsertMarker(const char* message)
{
    CHECK(mAllocator && "Invalid command list states.");
    CHECK(message && "Debug message must not be null");
    mCommandBuffer.insertDebugUtilsLabelEXT({.pLabelName = message});
    return *this;
}
RHICommandList& VulkanCommandList::DebugEnd()
{
    CHECK(mAllocator && "Invalid command list states.");
    mCommandBuffer.endDebugUtilsLabelEXT();
    return *this;
}

void VulkanCommandList::DebugSetObjectName(const char* name)
{
    VkCommandBuffer handle = *mCommandBuffer;
    mCommandPool.GetDevice().GetVkDevice().setDebugUtilsObjectNameEXT(
        {.objectType = vk::ObjectType::eCommandBuffer,
         .objectHandle = reinterpret_cast<uint64_t>(handle),
         .pObjectName = name});
}
