#pragma once
/* Implements RHI::Resource Containers on CPU Heaps (Upload Heaps) */
#include "../RHI/RHI.hpp"
template<typename T> concept PodDataType = std::is_trivially_copyable<T>::value;
template<PodDataType Type, bool IsRawBuffer = false> struct BufferContainer : public RHI::Resource {
	using RHI::Resource::GetNativeResource;
	using RHI::Resource::Release;
	using RHI::Resource::IsValid;
	BufferContainer(RHI::Device* device, uint numElements = 1, RHI::name_t name = nullptr) : RHI::Resource(device, Resource::ResourceDesc::GetGenericBufferDesc(
		sizeof(Type)* numElements, IsRawBuffer ? RAW_BUFFER_STRIDE : sizeof(Type)
	)) {
		pMappedData = Map(0); // Buffers only have one subresource
		SetName(name ? name : L"<Buffer>");
	}
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
	};	
	RHI::Resource::ResourceDesc const& GetDestTextureDesc() { return DstDesc; }
	const uint NumMips() { return m_Desc.mipLevels; }
	const uint NumSlices() { return m_Desc.arraySize; }	
	void WriteSubresource(void* srcData, uint srcPitch, uint srcHeight, uint dstArraySlice = 0, uint dstMipSlice = 0) {
		const D3D12_RESOURCE_DESC Desc = DstDesc;
		UINT SubresourceIndex = DstDesc.indexSubresource(dstArraySlice, dstMipSlice);
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT Layout;
		UINT NumRows;
		UINT64 RowSizeInBytes;
		UINT64 TotalBytes;
		m_Device->GetNativeDevice()->GetCopyableFootprints(&Desc, SubresourceIndex, 1, 0, &Layout, &NumRows, &RowSizeInBytes, &TotalBytes);
		D3D12_MEMCPY_DEST dest = { 
			(BYTE*)pMappedData + Layout.Offset, Layout.Footprint.RowPitch, SIZE_T(Layout.Footprint.RowPitch) * SIZE_T(NumRows)
		};		
		D3D12_SUBRESOURCE_DATA src{
			.pData = srcData,
			.RowPitch = srcPitch,
			.SlicePitch = 0
		};
		NumRows = std::min(NumRows, srcHeight);
		MemcpySubresource(&dest, &src, RowSizeInBytes, NumRows, 1);
	}
}; 