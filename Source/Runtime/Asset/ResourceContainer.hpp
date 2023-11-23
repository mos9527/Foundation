#pragma once
/* Implements RHI::Resource Containers on CPU Heaps (Upload Heaps) */
#include "../RHI/RHI.hpp"
template<typename T> concept PodDataType = requires (T a) {
	std::is_trivially_copyable<T>::value;
};
template<PodDataType Type, bool IsRawBuffer = false> struct BufferContainer : public RHI::Resource {
	using RHI::Resource::GetNativeResource;
	using RHI::Resource::Release;
	using RHI::Resource::IsValid;
	BufferContainer(RHI::Device* device, uint numElements = 1) : RHI::Resource(device, Resource::ResourceDesc::GetGenericBufferDesc(
		sizeof(Type)* numElements, IsRawBuffer ? RAW_BUFFER_STRIDE : sizeof(Type)
	)) {
		pMappedData = Map(0); // Buffers only have one subresource
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
	Texture2DContainer(RHI::Device* device, RHI::ResourceFormat format, uint width, uint height, uint numMips = 1, uint numSlices = 1) : RHI::Resource(
		device, RHI::Resource::ResourceDesc::GetTextureBufferDesc(
			format, RHI::ResourceDimension::Texture2D, width, height, numMips, numSlices, 1, 0,
			RHI::ResourceFlags::None, RHI::ResourceHeapType::Upload
		)
	) {};	
	const uint NumMips() { return m_Desc.mipLevels; }
	const uint NumSlices() { return m_Desc.arraySize; }	
	void* MapSubresource(uint arraySlice, uint mipSlice) {
		uint subresource = m_Desc.indexSubresource(arraySlice, mipSlice);
		return Map(subresource);
	}
	void WriteSubresource(void* srcData, uint srcPitch, uint srcHeight, uint dstArraySlice = 0, uint dstMipSlice = 0) {
		const D3D12_RESOURCE_DESC Desc = m_Desc;
		UINT SubresourceIndex = m_Desc.indexSubresource(dstArraySlice, dstMipSlice);
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT Layout;
		UINT NumRows;
		UINT64 RowSizeInBytes;
		UINT64 TotalBytes;
		m_Device->GetNativeDevice()->GetCopyableFootprints(&Desc, SubresourceIndex, 1, 0, &Layout, &NumRows, &RowSizeInBytes, &TotalBytes);
		D3D12_MEMCPY_DEST dest{
			.pData = MapSubresource(dstArraySlice,dstMipSlice),
			.RowPitch = Layout.Footprint.RowPitch,
			.SlicePitch = 0
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