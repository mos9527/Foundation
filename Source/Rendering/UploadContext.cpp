#include "UploadContext.hpp"
using namespace Foundation;
using namespace Foundation::Rendering;
UploadContext::UploadContext(RHIDevice* device, Allocator* allocator, size_t stagingBudget) :
    m_device(device), m_allocator(allocator), m_commandLists(allocator), m_fences(allocator),
    m_stagingBuffer(device, stagingBudget, allocator)
{
    m_queue = m_device->GetDeviceQueue(RHIDeviceQueueType::Graphics);
    m_commandPool =
        m_device->CreateCommandPool({.queue = RHIDeviceQueueType::Graphics, .type = RHICommandPoolType::Transient});
}
RHIDeviceFence* UploadContext::Upload(RHIBuffer* dst, Span<const char> data, size_t dstOffset, size_t alignment,
                                      RHIResourceAccess dst_access, RHIPipelineStage dst_stage)
{
    size_t offset = m_stagingBuffer.Write(data, alignment);
    auto& cmd = m_commandLists.emplace_back(m_commandPool->CreateCommandList());
    cmd->Begin();
    cmd->BeginTransition();
    cmd->SetBufferTransition(dst,
                             {.dst_access = RHIResourceAccessBits::TransferWrite,
                              .dst_stage = RHIPipelineStageBits::Transfer,
                              .src_buffer_offset = dstOffset,
                              .src_buffer_size = static_cast<uint32_t>(data.size())});
    cmd->EndTransition();
    cmd->CopyBuffer(m_stagingBuffer.GetBuffer(), dst,
                    {{{.src_offset = offset, .dst_offset = dstOffset, .size = data.size()}}});
    cmd->BeginTransition();
    cmd->SetBufferTransition(dst,
                             {.dst_access = dst_access,
                              .dst_stage = dst_stage,
                              .src_buffer_offset = dstOffset,
                              .src_buffer_size = static_cast<uint32_t>(data.size())});
    cmd->EndTransition();
    cmd->End();
    auto& fence = m_fences.emplace_back(m_device->CreateFence(false));
    m_queue->Submit({.cmd_lists = {{cmd.Get()}}, .fence = fence.Get()});
    return fence.Get();
}
RHIDeviceFence* UploadContext::Upload(RHITexture* dst, Span<const char> data, uint32_t mipLevel, uint32_t arrayLayer,
                                      RHITextureAspectFlag aspect, RHIResourceAccess dst_access,
                                      RHIPipelineStage dst_stage, RHITextureLayout dst_layout)
{
    size_t offset = m_stagingBuffer.Write(data, 4);
    auto& cmd = m_commandLists.emplace_back(m_commandPool->CreateCommandList());
    RHITextureSubresourceRange range = RHITextureSubresourceRange::Create(aspect, mipLevel, 1, arrayLayer, 1);
    cmd->Begin();
    cmd->BeginTransition();
    cmd->SetImageTransition(dst,
                            {.dst_access = RHIResourceAccessBits::TransferWrite,
                             .dst_stage = RHIPipelineStageBits::Transfer,
                             .dst_img_layout = RHITextureLayout::TransferDst,
                             .src_img_range = range});
    cmd->EndTransition();
    cmd->CopyBufferToImage(m_stagingBuffer.GetBuffer(), dst, RHITextureLayout::TransferDst,
                           {{{.src_buffer_offset = static_cast<uint32_t>(offset),
                              .dst_layer =
                                  {
                                      .aspect = aspect,
                                      .mip_level = mipLevel,
                                      .base_array_layer = arrayLayer,
                                      .layer_count = 1,
                                  },
                              .extent = dst->m_desc.extent}}});
    cmd->BeginTransition();
    cmd->SetImageTransition(
        dst, {.dst_access = dst_access, .dst_stage = dst_stage, .dst_img_layout = dst_layout, .src_img_range = range});
    cmd->EndTransition();
    cmd->End();
    auto& fence = m_fences.emplace_back(m_device->CreateFence(false));
    m_queue->Submit({.cmd_lists = {{cmd.Get()}}, .fence = fence.Get()});
    return fence.Get();
}
void UploadContext::WaitAll()
{
    LOG_RUNTIME(UploadContext, debug, "Waiting on {} resource uploads", m_fences.size());
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
