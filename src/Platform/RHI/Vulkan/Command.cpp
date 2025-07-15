#include <Platform/RHI/Vulkan/Command.hpp>
#include <Platform/RHI/Vulkan/Device.hpp>
using namespace Foundation::Platform::RHI;
VulkanCommandPool::VulkanCommandPool(const VulkanDevice& device, PoolDesc const& desc, Core::Allocator* allocator) :
    RHICommandPool(device, desc), m_device(device), m_allocator(allocator), m_storage(allocator, kCommandListReserveSize) {
    // Create the command pool
    auto const& queue = device.GetVkQueues()->Get(desc.queue);
    vk::CommandPoolCreateFlagBits flag{};
    switch (desc.type)
    {
    case RHICommandPoolType::Transient:
        flag = vk::CommandPoolCreateFlagBits::eTransient;
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
}

RHICommandList& VulkanCommandList::Begin() {
    CHECK(!m_allocator && "Invalid command list states. Did you call End()?");
    m_allocator.Reset(m_arena);
    m_commandBuffer.begin(vk::CommandBufferBeginInfo{});
    return *this;
}
RHICommandList& VulkanCommandList::BeginTransition() {
    m_barriers = Core::ConstructUnique<Barriers>(&m_allocator, &m_allocator);
    return *this;
}
#include <Platform/RHI/Vulkan/Resource.hpp>
RHICommandList& VulkanCommandList::SetBufferTransition(RHIBuffer* image, TransitionDesc const& desc) {
    CHECK(m_barriers && "Invalid barrier states. Did you call BeginTransition()?");
    auto* res = static_cast<VulkanBuffer*>(image);
    // !! TODO
    return *this;
}
RHICommandList& VulkanCommandList::SetImageTransition(RHIImage* image, TransitionDesc const& desc) {
    CHECK(m_barriers && "Invalid barrier states. Did you call BeginTransition()?");
    auto* res = static_cast<VulkanImage*>(image);
    m_barriers->image.push_back(vk::ImageMemoryBarrier2{
        .srcStageMask = vkPipelineStageFlags2FromRHIPipelineStage(desc.src_stage),
        .srcAccessMask = vkAccessFlagsFromRHIResourceAccess(desc.src_access),
        .dstStageMask = vkPipelineStageFlags2FromRHIPipelineStage(desc.dst_stage),
        .dstAccessMask = vkAccessFlagsFromRHIResourceAccess(desc.dst_access),
        .oldLayout = vkImageLayoutFromRHIImageLayout(desc.src_img_layout),
        .newLayout = vkImageLayoutFromRHIImageLayout(desc.dst_img_layout),
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = *res->GetVkImage(),
        .subresourceRange = {
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1
        }
    });
    return *this;
}
RHICommandList& VulkanCommandList::EndTransition() {
    CHECK(m_barriers && "Invalid barrier states. Did you call BeginTransition()?");
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

#include <Platform/RHI/Vulkan/PipelineState.hpp>
RHICommandList& VulkanCommandList::SetPipeline(PipelineDesc const& desc) {
    CHECK(m_allocator && "Invalid command list states. Did you call Begin()?");
    CHECK(desc.pipeline && "Pipeline is invalid.");
    auto* pipeline = static_cast<VulkanPipelineState*>(desc.pipeline);
    vk::PipelineBindPoint bindpoint = vk::PipelineBindPoint::eGraphics;
    switch (desc.type) {
    case PipelineDesc::PipelineType::Graphics:
        bindpoint = vk::PipelineBindPoint::eGraphics;
        break;
    case PipelineDesc::PipelineType::Compute:
        bindpoint = vk::PipelineBindPoint::eCompute;
        break;
    }
    m_commandBuffer.bindPipeline(bindpoint, *pipeline->GetVkPipeline());
    return *this;
}

RHICommandList& VulkanCommandList::SetViewport(float x, float y, float width, float height, float depth_min, float depth_max) {
    CHECK(m_allocator && "Invalid command list states. Did you call Begin()?");
    vk::Viewport viewport{ x, y, width, height, depth_min, depth_max };
    m_commandBuffer.setViewport(0, viewport);
    return *this;
}

RHICommandList& VulkanCommandList::SetScissor(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    CHECK(m_allocator && "Invalid command list states. Did you call Begin()?");
    vk::Rect2D scissor{ { (int32_t)x, (int32_t)y }, { width, height } };
    m_commandBuffer.setScissor(0, scissor);
    return *this;
}

RHICommandList& VulkanCommandList::Draw(uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance) {
    CHECK(m_allocator && "Invalid command list states. Did you call Begin()?");
    m_commandBuffer.draw(vertex_count, instance_count, first_vertex, first_instance);
    return *this;
}

RHICommandList& VulkanCommandList::BeginGraphics(GraphicsDesc const& desc) {
    CHECK(m_allocator && "Invalid command list states. Did you call Begin()?");
    std::vector<vk::RenderingAttachmentInfo, Core::StlAllocator<vk::RenderingAttachmentInfo>>
        attachments(&m_allocator);
    attachments.reserve(desc.attachments.size());    
    for (auto const& attachment : desc.attachments) {        
        attachments.push_back(vk::RenderingAttachmentInfo{
            .imageView = static_cast<const VulkanImageView*>(attachment.image_view)->GetVkImageView(),
            .imageLayout = vkImageLayoutFromRHIImageLayout(attachment.image_layout),
            .loadOp = vk::AttachmentLoadOp::eClear,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue = vk::ClearColorValue(attachment.clear_color.ToArray())
        });
    }
    vk::RenderingInfo renderingInfo{
        .renderArea = vk::Rect2D{ {0, 0}, vk::Extent2D{desc.width, desc.height} },
        .layerCount = 1,
        .colorAttachmentCount = static_cast<uint32_t>(attachments.size()),
        .pColorAttachments = attachments.data(),
    };
    m_commandBuffer.beginRendering(renderingInfo);
    return *this;
}

RHICommandList& VulkanCommandList::EndGraphics() {
    CHECK(m_allocator && "Invalid command list states. Did you call Begin()?");
    m_commandBuffer.endRendering();
    return *this;
}

void VulkanCommandList::End() {
    CHECK(m_allocator && "Invalid command list states. Did you call Begin()?");
    m_commandBuffer.end();
    m_allocator.Reset();
}
