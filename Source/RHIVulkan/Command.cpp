#include "Command.hpp"
#include "Device.hpp"

using namespace Foundation::RHI;
VulkanCommandPool::VulkanCommandPool(const VulkanDevice& device, PoolDesc const& desc, Allocator* allocator) :
    RHICommandPool(device, desc), m_allocator(allocator), m_device(device), m_storage(allocator) {
    // Create the command pool
    auto const& queue = device.GetVkQueues()->Get(desc.queue);
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
        .queueFamilyIndex = queue->GetVkQueueIndex(),
    };
    m_commandPool = vk::raii::CommandPool(device.GetVkDevice(), pool_info, m_device.GetVkAllocatorCallbacks());
    CHECK(m_commandPool != nullptr && "failed to create Vulkan command pool");
}

void VulkanCommandPool::DebugSetObjectName(const char* name) {
    VkCommandPool handle = *m_commandPool;
    m_device.GetVkDevice().setDebugUtilsObjectNameEXT({
        .objectType = vk::ObjectType::eCommandPool,
        .objectHandle = reinterpret_cast<uint64_t>(handle),
        .pObjectName = name
        });
}

RHICommandPoolScopedHandle<RHICommandList> VulkanCommandPool::CreateCommandList() {
    return { this, m_storage.CreateObject<VulkanCommandList>(*this) };
}
RHICommandList* VulkanCommandPool::GetCommandList(Handle handle) const {
    return m_storage.GetObjectPtr<RHICommandList>(handle);
}
void VulkanCommandPool::DestroyCommandList(Handle handle) {
    m_storage.DestroyObject(handle);
}

VulkanCommandList::VulkanCommandList(const VulkanCommandPool& commandPool) :
    RHICommandList(commandPool), m_commandPool(commandPool), m_arena(commandPool.GetDevice().GetAllocator(), kArenaSize) {
    vk::CommandBufferAllocateInfo alloc_info{
        .commandPool = *m_commandPool.GetVkCommandPool(),
        .level = vk::CommandBufferLevel::ePrimary,
        .commandBufferCount = 1,
    };
    m_commandBuffer = std::move(vk::raii::CommandBuffers(m_commandPool.GetDevice().GetVkDevice(), alloc_info).front());
    CHECK(m_commandBuffer != nullptr && "failed to allocate Vulkan command buffer");
}

RHICommandList& VulkanCommandList::Begin() {
    CHECK(!m_allocator.GetUsedMemory() && "Invalid command list states.");
    m_allocator.Reset(m_arena);
    m_commandBuffer.begin(vk::CommandBufferBeginInfo{});
    return *this;
}
RHICommandList& VulkanCommandList::BeginTransition() {
    CHECK(m_allocator && "Invalid command list states.");
    m_barriers = ConstructUnique<Barriers>(&m_allocator, &m_allocator);
    return *this;
}
#include "Resource.hpp"
RHICommandList& VulkanCommandList::SetBufferTransition(RHIBuffer* image, TransitionDesc const& desc) {
    CHECK(m_barriers && "Invalid barrier states.");
    auto* res = static_cast<VulkanBuffer*>(image);
    m_barriers->buffer.push_back(vk::BufferMemoryBarrier2{
        .srcStageMask = vkPipelineStageFlags2FromRHIPipelineStage(desc.src_stage),
        .srcAccessMask = vkAccessFlagsFromRHIResourceAccess(desc.src_access),
        .dstStageMask = vkPipelineStageFlags2FromRHIPipelineStage(desc.dst_stage),
        .dstAccessMask = vkAccessFlagsFromRHIResourceAccess(desc.dst_access),
        .srcQueueFamilyIndex = desc.src_queue_index,
        .dstQueueFamilyIndex = desc.dst_queue_index,
        .buffer = *res->GetVkBuffer(),
        .offset = desc.src_buffer_offset,
        .size = desc.src_buffer_size == kFullSize ? VK_WHOLE_SIZE : desc.src_buffer_size
        });
    return *this;
}
RHICommandList& VulkanCommandList::SetImageTransition(RHITexture* image, TransitionDesc const& desc) {
    CHECK(m_barriers && "Invalid barrier states.");
    CHECK_MSG(desc.src_img_range.layer.aspect.value, "Aspect flag on transition subresource is NULL!!");
    auto* res = static_cast<VulkanTexture*>(image);
    m_barriers->image.push_back(vk::ImageMemoryBarrier2{
        .srcStageMask = vkPipelineStageFlags2FromRHIPipelineStage(desc.src_stage),
        .srcAccessMask = vkAccessFlagsFromRHIResourceAccess(desc.src_access),
        .dstStageMask = vkPipelineStageFlags2FromRHIPipelineStage(desc.dst_stage),
        .dstAccessMask = vkAccessFlagsFromRHIResourceAccess(desc.dst_access),
        .oldLayout = vkImageLayoutFromRHITextureLayout(desc.src_img_layout),
        .newLayout = vkImageLayoutFromRHITextureLayout(desc.dst_img_layout),
        .srcQueueFamilyIndex = desc.src_queue_index,
        .dstQueueFamilyIndex = desc.dst_queue_index,
        .image = *res->GetVkImage(),
        .subresourceRange = {
            .aspectMask = vkImageAspectFlagFromRHITextureAspect(desc.src_img_range.layer.aspect),
            .baseMipLevel = desc.src_img_range.layer.mip_level,
            .levelCount = desc.src_img_range.mip_count,
            .baseArrayLayer = desc.src_img_range.layer.base_array_layer,
            .layerCount = desc.src_img_range.layer.layer_count
        }
        });
    return *this;
}
RHICommandList& VulkanCommandList::EndTransition() {
    CHECK(m_barriers && "Invalid barrier states.");
    if (m_barriers->image.empty() && m_barriers->buffer.empty())
        return *this; // No transitions to apply
    vk::DependencyInfo dependency_info{
        .bufferMemoryBarrierCount = static_cast<uint32_t>(m_barriers->buffer.size()),
        .pBufferMemoryBarriers = m_barriers->buffer.data(),
        .imageMemoryBarrierCount = static_cast<uint32_t>(m_barriers->image.size()),
        .pImageMemoryBarriers = m_barriers->image.data(),
    };
    m_commandBuffer.pipelineBarrier2(dependency_info);
    m_barriers.reset();
    return *this;
}

#include "PipelineState.hpp"
RHICommandList& VulkanCommandList::SetPipeline(PipelineDesc const& desc) {
    CHECK(m_allocator && "Invalid command list states.");
    CHECK(desc.pipeline && "Pipeline is invalid.");
    m_commandBuffer.bindPipeline(
        vkPipelineBindPointFromRHIDevicePipelineType(desc.type),
        *static_cast<VulkanPipelineState*>(desc.pipeline)->GetVkPipeline()
    );
    return *this;
}

RHICommandList& VulkanCommandList::SetViewport(float x, float y, float width, float height, float depth_min, float depth_max) {
    CHECK(m_allocator && "Invalid command list states.");
    vk::Viewport viewport{ x, y, width, height, depth_min, depth_max };
    m_commandBuffer.setViewport(0, viewport);
    return *this;
}

RHICommandList& VulkanCommandList::SetScissor(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    CHECK(m_allocator && "Invalid command list states.");
    vk::Rect2D scissor{ { static_cast<int32_t>(x), static_cast<int32_t>(y) }, { width, height } };
    m_commandBuffer.setScissor(0, scissor);
    return *this;
}

RHICommandList& VulkanCommandList::Draw(uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance) {
    CHECK(m_allocator && "Invalid command list states.");
    m_commandBuffer.draw(vertex_count, instance_count, first_vertex, first_instance);
    return *this;
}

RHICommandList& VulkanCommandList::DrawIndexed(uint32_t index_count, uint32_t instance_count, uint32_t first_index, int32_t vertex_offset, uint32_t first_instance) {
    CHECK(m_allocator && "Invalid command list states.");
    m_commandBuffer.drawIndexed(index_count, instance_count, first_index, vertex_offset, first_instance);
    return *this;
}

RHICommandList& VulkanCommandList::DrawIndexedIndirectCount(RHIBuffer* buffer, size_t offset, RHIBuffer* count_buffer, size_t count_offset, uint32_t max_draw_count, uint32_t stride)
{
    CHECK(m_allocator && "Invalid command list states.");
    m_commandBuffer.drawIndexedIndirectCount(
        static_cast<VulkanBuffer*>(buffer)->GetVkBuffer(),
        offset,
        static_cast<VulkanBuffer*>(count_buffer)->GetVkBuffer(),
        count_offset,
        max_draw_count,
        stride
    );
    return *this;
}

RHICommandList& VulkanCommandList::PushConstant(RHIPipelineState* pipeline, RHIShaderStage stage, uint32_t offset,
                                                Span<const char> data)
{
    CHECK(m_allocator && "Invalid command list states.");
    vkCmdPushConstants(*m_commandBuffer, *static_cast<VulkanPipelineState*>(pipeline)->GetVkPipelineLayout(),
                       static_cast<VkShaderStageFlags>(vkShaderStageFlagsFromRHIShaderStage(stage)), offset,
                       data.size(), data.data());
    return *this;
}
RHICommandList& VulkanCommandList::FillBuffer(RHIBuffer* buffer, uint32_t value, size_t offset, size_t size)
{
    CHECK(m_allocator && "Invalid command list states.");
    auto* vulkan_buffer = static_cast<VulkanBuffer*>(buffer);
    m_commandBuffer.fillBuffer(*vulkan_buffer->GetVkBuffer(), offset, size == kFullSize ? VK_WHOLE_SIZE : size, value);
    return *this;
}

RHICommandList& VulkanCommandList::CopyBuffer(RHIBuffer* src_buffer, RHIBuffer* dst_buffer, Span<const CopyBufferRegion> regions) {
    CHECK(m_allocator && "Invalid command list states.");
    CHECK(src_buffer && dst_buffer && "Source and destination buffers must be valid.");

    auto* src_vulkan_buffer = static_cast<VulkanBuffer*>(src_buffer);
    auto* dst_vulkan_buffer = static_cast<VulkanBuffer*>(dst_buffer);

    Vector<vk::BufferCopy> vk_regions(&m_allocator);
    for (auto const& region : regions) {
        size_t size = region.size;
        if (size == kFullSize) {
            size = std::min(
                src_vulkan_buffer->m_desc.size - region.src_offset,
                dst_vulkan_buffer->m_desc.size - region.dst_offset
            );
        }
        vk_regions.push_back(vk::BufferCopy{
            .srcOffset = static_cast<vk::DeviceSize>(region.src_offset),
            .dstOffset = static_cast<vk::DeviceSize>(region.dst_offset),
            .size = size
            });
    }
    m_commandBuffer.copyBuffer(*src_vulkan_buffer->GetVkBuffer(), *dst_vulkan_buffer->GetVkBuffer(), vk_regions);
    return *this;
}

RHICommandList& VulkanCommandList::CopyImage(RHITexture* src_image, RHITextureLayout src_layout, RHITexture* dst_image, RHITextureLayout dst_layout, Span<const CopyImageRegion> regions)
{
    CHECK(m_allocator && "Invalid command list states.");
    CHECK(src_image && dst_image && "Source and destination images must be valid.");

    auto* src_vulkan_image = static_cast<VulkanTexture*>(src_image);
    auto* dst_vulkan_image = static_cast<VulkanTexture*>(dst_image);

    Vector<vk::ImageCopy> vk_regions(&m_allocator);
    vk_regions.reserve(regions.size());
    for (auto const& region : regions) {
        vk_regions.push_back(vk::ImageCopy{
            .srcSubresource = {
                .aspectMask = vkImageAspectFlagFromRHITextureAspect(region.src_layer.aspect),
                .mipLevel = region.src_layer.mip_level,
                .baseArrayLayer = region.src_layer.base_array_layer,
                .layerCount = region.src_layer.layer_count
            },
            .srcOffset = { region.src_offset.x, region.src_offset.y, region.src_offset.z },
            .dstSubresource = {
                .aspectMask = vkImageAspectFlagFromRHITextureAspect(region.dst_layer.aspect),
                .mipLevel = region.dst_layer.mip_level,
                .baseArrayLayer = region.dst_layer.base_array_layer,
                .layerCount = region.dst_layer.layer_count
            },
            .dstOffset = { region.dst_offset.x, region.dst_offset.y, region.dst_offset.z },
            .extent = { region.extent.x, region.extent.y, region.extent.z }
            });
    }

    m_commandBuffer.copyImage(
        *src_vulkan_image->GetVkImage(), vkImageLayoutFromRHITextureLayout(src_layout),
        *dst_vulkan_image->GetVkImage(), vkImageLayoutFromRHITextureLayout(dst_layout),
        vk_regions
    );

    return *this;
}

RHICommandList& VulkanCommandList::CopyBufferToImage(RHIBuffer* src_buffer, RHITexture* dst_image, RHITextureLayout dst_layout, Span<const CopyImageRegion> regions)
{
    CHECK(m_allocator && "Invalid command list states.");
    CHECK(src_buffer && dst_image && "Source buffer and destination image must be valid.");

    auto* src_vulkan_buffer = static_cast<VulkanBuffer*>(src_buffer);
    auto* dst_vulkan_image = static_cast<VulkanTexture*>(dst_image);

    Vector<vk::BufferImageCopy> vk_regions(&m_allocator);
    vk_regions.reserve(regions.size());
    for (auto const& region : regions) {
        vk_regions.push_back(vk::BufferImageCopy{
            .bufferOffset = region.src_buffer_offset,
            .bufferRowLength = 0, // Tightly packed
            .bufferImageHeight = 0, // Tightly packed
            .imageSubresource = {
                .aspectMask = vkImageAspectFlagFromRHITextureAspect(region.dst_layer.aspect),
                .mipLevel = region.dst_layer.mip_level,
                .baseArrayLayer = region.dst_layer.base_array_layer,
                .layerCount = region.dst_layer.layer_count
            },
            .imageOffset = { region.dst_offset.x, region.dst_offset.y, region.dst_offset.z },
            .imageExtent = { region.extent.x, region.extent.y, region.extent.z }
            });
    }

    m_commandBuffer.copyBufferToImage(
        *src_vulkan_buffer->GetVkBuffer(),
        *dst_vulkan_image->GetVkImage(),
        vkImageLayoutFromRHITextureLayout(dst_layout),
        vk_regions
    );

    return *this;
}

RHICommandList& VulkanCommandList::BeginGraphics(GraphicsDesc const& desc) {
    CHECK(m_allocator && "Invalid command list states.");
    auto vkRenderingAttachmentInfoFromAttachment = [](const GraphicsDesc::Attachment& attachment) -> vk::RenderingAttachmentInfo {
        vk::ClearValue clear_value{ .color = vk::ClearColorValue{} };
        if (attachment.clear_color) {
            clear_value.color = vk::ClearColorValue{
                std::array{
                    attachment.clear_color->r,
                    attachment.clear_color->g,
                    attachment.clear_color->b,
                    attachment.clear_color->a
                }
            };
        }
        else if (attachment.clear_depth_stencil) {
            clear_value.depthStencil = vk::ClearDepthStencilValue{
                attachment.clear_depth_stencil->first,
                attachment.clear_depth_stencil->second
            };
        }
        return vk::RenderingAttachmentInfo{
            .imageView = attachment.image_view ? static_cast<const VulkanTextureView*>(attachment.image_view)->GetVkImageView() : vk::ImageView{ nullptr },
            .imageLayout = vkImageLayoutFromRHITextureLayout(attachment.image_layout),
            .loadOp = vk::AttachmentLoadOp::eClear,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue = clear_value
        };
        };
    Vector<vk::RenderingAttachmentInfo> attachments(&m_allocator);
    attachments.reserve(desc.color_attachments.size());
    for (auto const& attachment : desc.color_attachments) {
        attachments.push_back(vkRenderingAttachmentInfoFromAttachment(attachment));
    }
    vk::RenderingAttachmentInfo depth_attachment = vkRenderingAttachmentInfoFromAttachment(desc.depth_attachment);
    vk::RenderingAttachmentInfo stencil_attachment = vkRenderingAttachmentInfoFromAttachment(desc.stencil_attachment);
    vk::RenderingInfo renderingInfo{
        .renderArea = vk::Rect2D{ {0, 0}, vk::Extent2D{desc.width, desc.height} },
        .layerCount = 1,
        .colorAttachmentCount = static_cast<uint32_t>(attachments.size()),
        .pColorAttachments = attachments.data(),
        .pDepthAttachment = desc.depth_attachment.IsValid() ? &depth_attachment : nullptr,
        .pStencilAttachment = desc.stencil_attachment.IsValid() ? &stencil_attachment : nullptr,
    };
    m_commandBuffer.beginRendering(renderingInfo);
    return *this;
}

RHICommandList& VulkanCommandList::BindVertexBuffer(uint32_t index, Span<RHIBuffer* const> buffers, Span<const size_t> offsets) {
    CHECK(m_allocator && "Invalid command list states.");
    CHECK(buffers.size() == offsets.size() && "Buffers and offsets must have the same size.");

    Vector<vk::Buffer> vk_buffers(&m_allocator);
    Vector<vk::DeviceSize> vk_offsets(&m_allocator);
    for (size_t i = 0; i < buffers.size(); ++i) {
        auto* buffer = static_cast<VulkanBuffer*>(buffers[i]);
        vk_buffers.push_back(*buffer->GetVkBuffer());
        vk_offsets.push_back(static_cast<vk::DeviceSize>(offsets[i]));
    }
    m_commandBuffer.bindVertexBuffers(index, vk_buffers, vk_offsets);
    return *this;
}

RHICommandList& VulkanCommandList::BindIndexBuffer(RHIBuffer* buffer, size_t offset, RHIResourceFormat format) {
    CHECK(m_allocator && "Invalid command list states.");
    CHECK(buffer && "Buffer must be valid.");
    vk::IndexType type;
    switch (format)
    {
    case RHIResourceFormat::R32_UINT:
        type = vk::IndexType::eUint32; break;
    case RHIResourceFormat::R16_UINT:
        type = vk::IndexType::eUint16; break;
    default:
        throw std::runtime_error("unsupported index format");
    }
    auto* vulkan_buffer = static_cast<VulkanBuffer*>(buffer);
    m_commandBuffer.bindIndexBuffer(*vulkan_buffer->GetVkBuffer(), static_cast<vk::DeviceSize>(offset), type);
    return *this;
}

#include "Descriptor.hpp"
RHICommandList& VulkanCommandList::BindDescriptorSet(
    RHIDevicePipelineType bindpoint,
    RHIPipelineState* pipeline,
    Span<RHIDeviceDescriptorSet* const> sets,
    size_t first
) {
    CHECK(m_allocator && "Invalid command list states.");
    Vector<vk::DescriptorSet> vk_sets(&m_allocator);
    for (auto* set : sets) {
        CHECK(set && "Descriptor set must be valid.");
        auto* vulkan_set = static_cast<VulkanDeviceDescriptorSet*>(set);
        vk_sets.push_back(*(vulkan_set->GetVkDescriptorSet()));
    }
    m_commandBuffer.bindDescriptorSets(
        vkPipelineBindPointFromRHIDevicePipelineType(bindpoint),
        static_cast<VulkanPipelineState*>(pipeline)->GetVkPipelineLayout(),
        static_cast<uint32_t>(first),
        vk_sets,
        {} // TODO? Dynamic offsets
    );
    return *this;
}

RHICommandList& VulkanCommandList::EndGraphics() {
    CHECK(m_allocator && "Invalid command list states.");
    m_commandBuffer.endRendering();
    return *this;
}

RHICommandList& VulkanCommandList::Dispatch(uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z)
{
    CHECK(m_allocator && "Invalid command list states.");
    CHECK_MSG(group_count_x > 0 && group_count_y > 0 && group_count_z > 0, "Dispatch group counts must be greater than zero.");
    m_commandBuffer.dispatch(group_count_x, group_count_y, group_count_z);
    return *this;
}

void VulkanCommandList::End() {
    CHECK(m_allocator && "Invalid command list states.");
    m_commandBuffer.end();
    m_allocator.Reset();
}

void VulkanCommandList::Reset() {
    m_allocator.Reset(m_arena);
    m_allocator.Reset(m_arena);
    m_commandBuffer.reset();
}

RHICommandList& VulkanCommandList::DebugBegin(const char* message){
    CHECK(m_allocator && "Invalid command list states.");
    CHECK(message && "Debug message must not be null");
    m_commandBuffer.beginDebugUtilsLabelEXT({
        .pLabelName = message
    });
    return *this;
}
RHICommandList& VulkanCommandList::DebugInsertMarker(const char* message){
    CHECK(m_allocator && "Invalid command list states.");
    CHECK(message && "Debug message must not be null");
    m_commandBuffer.insertDebugUtilsLabelEXT({
        .pLabelName = message
    });
    return *this;
}
RHICommandList& VulkanCommandList::DebugEnd() {
    CHECK(m_allocator && "Invalid command list states.");
    m_commandBuffer.endDebugUtilsLabelEXT();
    return *this;
}

void VulkanCommandList::DebugSetObjectName(const char* name) {
    VkCommandBuffer handle = *m_commandBuffer;
    m_commandPool.GetDevice().GetVkDevice().setDebugUtilsObjectNameEXT({
        .objectType = vk::ObjectType::eCommandBuffer,
        .objectHandle = reinterpret_cast<uint64_t>(handle),
        .pObjectName = name
        });
}
