#include "Upload.hpp"
using namespace Foundation;
using namespace Foundation::Rendering;
UploadContext::UploadContext(RHIDevice* device, Allocator* allocator, size_t stagingBudget):
m_device(device), m_allocator(allocator), m_commandLists(allocator), m_fences(allocator),
m_stagingBuffer(device, stagingBudget, allocator)
{
    m_transferQueue = m_device->GetDeviceQueue(RHIDeviceQueueType::Transfer);
    m_commandPool = m_device->CreateCommandPool({
        .queue = RHIDeviceQueueType::Transfer,
        .type = RHICommandPoolType::Transient
    });
}
RHIDeviceFence* UploadContext::Upload(RHIBuffer* dst, Span<const char> data, size_t dstOffset, size_t alignment)
{
    size_t offset = m_stagingBuffer.Write(data, alignment);
    auto& cmd = m_commandLists.emplace_back(m_commandPool->CreateCommandList());
    cmd->Begin();
    cmd->CopyBuffer(m_stagingBuffer.GetBuffer(), dst, {{{
        .src_offset = offset, .dst_offset = dstOffset, .size = data.size()
    }}});
    cmd->End();
    auto& fence = m_fences.emplace_back(m_device->CreateFence(false));
    m_transferQueue->Submit({
        .cmd_lists = {{ cmd.Get() }},
        .fence = fence.Get()
    });
    return fence.Get();
}
RHIDeviceFence* UploadContext::Upload(RHITexture* dst, Span<const char> data, uint32_t mipLevel, uint32_t arrayLayer,
                                      RHITextureAspectFlag aspect)
{
    size_t offset = m_stagingBuffer.Write(data, 4);
    auto& cmd = m_commandLists.emplace_back(m_commandPool->CreateCommandList());
    cmd->Begin();
    cmd->CopyBufferToImage(m_stagingBuffer.GetBuffer(), dst, RHITextureLayout::TransferDst, {
        {RHICommandList::CopyImageRegion{
            .src_buffer_offset = static_cast<uint32_t>(offset),
            .dst_layer = {
                .aspect = aspect,
                .mip_level = mipLevel,
                .base_array_layer = arrayLayer,
                .layer_count = 1,
            },
            .extent = dst->m_desc.extent
        }}
    });
    cmd->End();
    auto& fence = m_fences.emplace_back(m_device->CreateFence(false));
    m_transferQueue->Submit({
        .cmd_lists = {{ cmd.Get() }},
        .fence = fence.Get()
    });
    return fence.Get();
}
void UploadContext::WaitAll()
{
    m_device->WaitForFences(m_fences, true, ~0ull);
    m_fences.clear();
    m_commandLists.clear();
    m_stagingBuffer.Reset();
}
UploadContext::~UploadContext()
{
    if (!m_fences.empty())
        WaitAll();
}
