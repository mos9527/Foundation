#pragma once
/* Implements RHI::Resource Containers on CPU Heaps (Upload Heaps) */
#include "../RHI/RHI.hpp"
template<typename T> concept PodDataType = std::is_trivially_copyable<T>::value;
// Persistently mapped CPU-Heap buffer
template<PodDataType Type, bool IsRawBuffer = false> struct BufferContainer : public RHI::Resource {
	using RHI::Resource::GetNativeResource;
	using RHI::Resource::Release;
	using RHI::Resource::IsValid;
	BufferContainer(RHI::Device* device, uint numElements = 1, RHI::name_t name = nullptr) : RHI::Resource(device, Resource::ResourceDesc::GetGenericBufferDesc(
		sizeof(Type)* numElements, IsRawBuffer ? RAW_BUFFER_STRIDE : sizeof(Type)
	)) {
		SetName(name ? name : L"<Buffer>");
		Map();
	}
	void Map() { pMappedData = RHI::Resource::Map(0); }
	void Unmap() { RHI::Resource::Unmap(0); pMappedData = nullptr; }
	const uint SizeInBytes() const { return m_Desc.sizeInBytes(); }
	const uint NumElements() const { return m_Desc.numElements(); }
	inline Type* Data() const { return reinterpret_cast<Type*>(pMappedData); }
	inline Type* DataAt(uint index) { return Data() + index; }
	void WriteData(Type* srcData, uint srcElemOffset = 0, uint srcNumElements = 1, uint dstElemOffset = 0) {
		memcpy(Data() + dstElemOffset, srcData + srcElemOffset, srcNumElements * sizeof(Type));
	};
private:
	void* pMappedData;
};
struct Texture2DContainer : public RHI::Resource {
	using RHI::Resource::GetNativeResource;
	using RHI::Resource::Release;
	using RHI::Resource::IsValid;
private:	
	std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
	std::vector<UINT> numRows;
	std::vector<UINT64> rowSizes;

	void PrepareTempBuffers(uint numSubresource) {
		footprints.resize(numSubresource);
		rowSizes.resize(numSubresource);
		numRows.resize(numSubresource);
	}

	RHI::Resource::ResourceDesc DstDesc;
	void* pMappedData;
	static size_t GetCopyableFootprintRequiredSize(ID3D12Device* device, D3D12_RESOURCE_DESC Desc, uint FirstSubresource, uint NumSubresources) {
		size_t RequiredSize;
		device->GetCopyableFootprints(&Desc, FirstSubresource, NumSubresources, 0, nullptr, nullptr, nullptr, &RequiredSize);
		return RequiredSize;
	}
public:
	Texture2DContainer(RHI::Device* device, RHI::Resource::ResourceDesc DstDesc, RHI::name_t name = nullptr) :
		DstDesc(DstDesc),
		RHI::Resource(
			device, RHI::Resource::ResourceDesc::GetGenericBufferDesc(GetCopyableFootprintRequiredSize(*device, DstDesc,0, DstDesc.numSubresources()))
		) {
		pMappedData = Map(0);
		SetName(name ? name : L"<Texture Upload Buffer>");
		PrepareTempBuffers(DstDesc.numSubresources());
		const D3D12_RESOURCE_DESC Desc = DstDesc;
		m_Device->GetNativeDevice()->GetCopyableFootprints(&Desc, 0, DstDesc.numSubresources(), 0, footprints.data(), numRows.data(), rowSizes.data(), NULL);
	};	
	RHI::Resource::ResourceDesc const& GetDestTextureDesc() { return DstDesc; }
	const uint NumMips() { return m_Desc.mipLevels; }
	const uint NumSlices() { return m_Desc.arraySize; }	
	void WriteSubresource(void* srcData, uint srcPitch, uint srcHeight, uint dstArraySlice = 0, uint dstMipSlice = 0) {
		const D3D12_RESOURCE_DESC Desc = DstDesc;
		CHECK(dstArraySlice < Desc.DepthOrArraySize && dstMipSlice < Desc.MipLevels);
		UINT SubresourceIndex = DstDesc.indexSubresource(dstArraySlice, dstMipSlice);				
		D3D12_MEMCPY_DEST dest = { 
			(BYTE*)pMappedData + footprints[SubresourceIndex].Offset, footprints[SubresourceIndex].Footprint.RowPitch, SIZE_T(footprints[SubresourceIndex].Footprint.RowPitch)* SIZE_T(numRows[SubresourceIndex])
		};		
		D3D12_SUBRESOURCE_DATA src{
			.pData = srcData,
			.RowPitch = srcPitch,
			.SlicePitch = 0
		};		
		MemcpySubresource(&dest, &src, rowSizes[SubresourceIndex], std::min(numRows[SubresourceIndex], srcHeight), 1);
	}
}; 