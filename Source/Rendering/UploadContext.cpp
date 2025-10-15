#include "UploadContext.hpp"
using namespace Foundation;
using namespace Foundation::Rendering;
UploadContext::UploadContext(RHIDevice* device, Allocator* allocator, size_t stagingBudget) :
    mDevice(device), mAllocator(allocator), mCommandLists(allocator), mStagingBuffer(device, stagingBudget, allocator)
{
    mQueue = mDevice->GetDeviceQueue(RHIDeviceQueueType::Graphics);
    mCommandPool =
        mDevice->CreateCommandPool({.queue = RHIDeviceQueueType::Graphics, .type = RHICommandPoolType::Transient});
    mFence = mDevice->CreateFence(true);
}
void UploadContext::Upload(RHIBuffer* dst, Span<const char> data, size_t dstOffset, size_t alignment,
                           RHIResourceAccess dst_access, RHIPipelineStage dst_stage)
{
    std::scoped_lock lock(mMutex);
    size_t offset = mStagingBuffer.Write(data, alignment);
    auto& cmd = mCommandLists.emplace_back(mCommandPool->CreateCommandList());
    cmd->Begin();
    cmd->BeginTransition();
    cmd->SetBufferTransition(dst,
                             {.dstAccess = RHIResourceAccessBits::TransferWrite,
                              .dstStage = RHIPipelineStageBits::Transfer,
                              .srcBufferOffset = dstOffset,
                              .srcBufferSize = static_cast<uint32_t>(data.size())});
    cmd->EndTransition();
    cmd->CopyBuffer(mStagingBuffer.GetBuffer(), dst,
                    {{{.srcOffset = offset, .dstOffset = dstOffset, .size = data.size()}}});
    cmd->BeginTransition();
    cmd->SetBufferTransition(dst,
                             {.dstAccess = dst_access,
                              .dstStage = dst_stage,
                              .srcBufferOffset = dstOffset,
                              .srcBufferSize = static_cast<uint32_t>(data.size())});
    cmd->EndTransition();
    cmd->End();
}
void UploadContext::Upload(RHITexture* dst, Span<const char> data, uint32_t mipLevel, uint32_t arrayLayer,
                           RHITextureAspectFlag aspect, RHIResourceAccess dst_access, RHIPipelineStage dst_stage,
                           RHITextureLayout dst_layout)
{
    RHITextureSubresourceRange range = RHITextureSubresourceRange::Create(aspect, mipLevel, 1, arrayLayer, 1);
    RHICommandList::CopyImageRegion region{.dstLayer = range.layer, .extent = dst->mDesc.extent};
    Upload(dst, data, range, region, dst_access, dst_stage, dst_layout);
}
void UploadContext::Upload(RHITexture* dst, Span<const char> data, RHITextureSubresourceRange range,
                           RHICommandList::CopyImageRegion region, RHIResourceAccess dst_access,
                           RHIPipelineStage dst_stage, RHITextureLayout dst_layout)
{
    std::scoped_lock lock(mMutex);
    size_t offset = mStagingBuffer.Write(data, 4);
    region.srcBufferOffset = offset;
    auto& cmd = mCommandLists.emplace_back(mCommandPool->CreateCommandList());
    cmd->Begin();
    cmd->BeginTransition();
    cmd->SetImageTransition(dst,
                            {.dstAccess = RHIResourceAccessBits::TransferWrite,
                             .dstStage = RHIPipelineStageBits::Transfer,
                             .dstImgLayout = RHITextureLayout::TransferDst,
                             .srcImgRange = range});
    cmd->EndTransition();
    cmd->CopyBufferToImage(mStagingBuffer.GetBuffer(), dst, RHITextureLayout::TransferDst, {{region}});
    cmd->BeginTransition();
    cmd->SetImageTransition(dst,
                            {.srcAccess = RHIResourceAccessBits::TransferWrite,
                             .dstAccess = dst_access,
                             .srcStage = RHIPipelineStageBits::Transfer,
                             .dstStage = dst_stage,
                             .dstImgLayout = dst_layout,
                             .srcImgRange = range});
    cmd->EndTransition();
    cmd->End();
}
void UploadContext::SubmitAndWait()
{
    std::scoped_lock lock(mMutex);
    if (mCommandLists.empty())
        return;
    mDevice->WaitForFences({{{mFence}}}, true, ~0ull);
    mDevice->ResetFences({{{mFence}}});
    Vector<RHICommandList*> cmds(mAllocator);
    for (auto& cmd : mCommandLists)
        cmds.push_back(cmd.Get());
    mQueue->Submit({{{
                       .cmdLists = cmds,
                   }}},
                   mFence.Get());
    mDevice->WaitForFences({{{mFence}}}, true, ~0ull);
    mCommandLists.clear();
    mStagingBuffer.Reset();
}
UploadContext::~UploadContext() { SubmitAndWait(); }
