#include "UploadContext.hpp"
using namespace RHI;
Texture2DContainer* UploadContext::CreateIntermediateTexture2DContainer(Resource* dst) {
    auto& intermediate = m_IntermediateBuffers.emplace_back(std::make_unique<Texture2DContainer>(this->GetParent(), dst->GetDesc()));
    return static_cast<Texture2DContainer*>(intermediate.get());
}
void UploadContext::QueueUploadTexture(Resource* dst, Resource* intermediate, uint FirstSubresource, uint NumSubresources) {
    CHECK(dst->GetDesc().dimension == ResourceDimension::Texture2D);
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
void UploadContext::QueueUploadBuffer(Resource* dst, void* data, uint sizeInBytes) {
    CHECK(dst->GetDesc().dimension == ResourceDimension::Buffer);
    std::scoped_lock lock(m_UploadMutex);
    PrepareTempBuffers(1);
    size_t RequiredSize = GetRequiredIntermediateSize(*dst, 0, 1);
    auto& intermediate = m_IntermediateBuffers.emplace_back(std::make_unique<Resource>(this->GetParent(), Resource::ResourceDesc::GetGenericBufferDesc(RequiredSize)));
    Subresource subresource{
        .pData = data,
        .RowPitch = sizeInBytes,
        .SlicePitch = sizeInBytes
    };
    UpdateSubresources(
        *this,
        *dst,
        *intermediate,
        0,
        1,
        RequiredSize,
        m_FootprintBuffer.data(),
        m_RowsBuffer.data(),
        m_RowSizesBuffer.data(),
        &subresource
    );
};
SyncFence UploadContext::End() {
    CommandList::FlushBarriers();
    CommandList::Close();
    return CommandList::Execute();
}