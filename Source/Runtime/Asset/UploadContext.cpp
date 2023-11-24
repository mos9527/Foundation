#include "UploadContext.hpp"
using namespace RHI;
void UploadContext::QueueUpload(Resource* dst, Resource* intermediate, uint FirstSubresource, uint NumSubresources) {
    std::scoped_lock lock(m_UploadMutex);
    PrepareTempBuffers(NumSubresources);
    const D3D12_RESOURCE_DESC Desc = dst->GetDesc();
    size_t RequiredSize;
    m_Device->GetNativeDevice()->GetCopyableFootprints(&Desc, FirstSubresource, NumSubresources, 0, m_FootprintBuffer.data(), NULL, NULL, &RequiredSize);
    CHECK(RequiredSize <= intermediate->GetDesc().sizeInBytes() && "Intermediate out-of-bounds");
    for (UINT i = 0; i < NumSubresources; ++i)
    {
        const CD3DX12_TEXTURE_COPY_LOCATION Dst(*dst, i + FirstSubresource);
        const CD3DX12_TEXTURE_COPY_LOCATION Src(*intermediate, m_FootprintBuffer[i]);
        GetNativeCommandList()->CopyTextureRegion(&Dst, 0, 0, 0, &Src, nullptr);
    }
};
void UploadContext::QueueUpload(Resource* dst, Subresource* data, uint FirstSubresource, uint NumSubresources) {
    std::scoped_lock lock(m_UploadMutex);
    PrepareTempBuffers(NumSubresources);
    size_t RequiredSize = GetRequiredIntermediateSize(*dst, FirstSubresource, NumSubresources);
    auto& intermediate = m_IntermediateBuffers.emplace_back(std::make_unique<Resource>(this->GetParent(), Resource::ResourceDesc::GetGenericBufferDesc(RequiredSize)));
    UpdateSubresources(
        *this,
        *dst,
        *intermediate,
        FirstSubresource,
        NumSubresources,
        RequiredSize,
        m_FootprintBuffer.data(),
        m_RowsBuffer.data(),
        m_RowSizesBuffer.data(),
        data
    );
};
void UploadContext::QueueUpload(Resource* dst, void* data, uint sizeInBytes) {
    Subresource subresource{
        .pData = data,
        .RowPitch = sizeInBytes,
        .SlicePitch = sizeInBytes
    };
    QueueUpload(dst, &subresource, 1);
}
SyncFence UploadContext::End() {
    CommandList::FlushBarriers();
    CommandList::Close();
    return CommandList::Execute();
}