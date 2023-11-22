#include "AssetUploadContext.hpp"
using namespace RHI;
void AssetUploadContext::Upload(Resource* dst, Subresource* data, uint FirstSubresource, uint NumSubresources) {
    std::scoped_lock lock(uploadMutex);
    prepare_temp_buffers(NumSubresources);
    const D3D12_RESOURCE_DESC Desc = dst->GetDesc();
    size_t RequiredSize;    
    m_Device->GetNativeDevice()->GetCopyableFootprints(&Desc, FirstSubresource, NumSubresources, 0, NULL, NULL, NULL, &RequiredSize);
    auto& intermediate = intermediateBuffers.emplace_back(std::make_unique<Resource>(this, Resource::ResourceDesc::GetGenericBufferDesc(RequiredSize)));    
    UpdateSubresources(
        *this,
        *dst,
        *intermediate,
        FirstSubresource,
        NumSubresources,
        RequiredSize,
        FootprintBuffer.data(),
        RowsBuffer.data(),
        RowSizesBuffer.data(),
        data
    );
};
void AssetUploadContext::Upload(Resource* dst, void* data, uint sizeInBytes) {
    Subresource subresource{
        .pData = data,
        .RowPitch = sizeInBytes,
        .SlicePitch = sizeInBytes
    };
    Upload(dst, &subresource, 1);
}
