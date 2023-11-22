#pragma once
/* Implements STL-like containers with simplified RHI::Resource in its Upload state */
#include "RHI.hpp"
template<typename Type, bool IsRawBuffer = false> struct BufferUploadContainer : private RHI::Buffer {
	using RHI::Buffer::~Buffer;
	BufferUploadContainer(RHI::Device* device, size_t numElements = 1) : RHI::Buffer(device, Resource::ResourceDesc::GetGenericBufferDesc(
		sizeof(Type)* numElements, IsRawBuffer ? RAW_BUFFER_STRIDE : sizeof(Type)
	)) {
		pMappedData = Map(0); // Buffers only have one subresource
	}
	const size_t size_in_bytes() const { return m_Desc.sizeInBytes(); }
	const size_t size() const { return m_Desc.numElements(); }
	const inline Type* data() const { return reinterpret_cast<Type*>(pMappedData); }
	Type& inline at(size_t index) { return *(data() + (Type*)index); }
	Type& inline operator[](size_t index) {
		assert(index < size());
		return at(index);
	}
	const inline Type& operator[](size_t index) const {
		return at(index);
	}
private:
	void* pMappedData;
};
struct Texture2DUploadContainer : private RHI::Texture {
	using RHI::Texture::~Texture;
	Texture2DUploadContainer(RHI::Device* device, RHI::ResourceFormat format, uint width, uint height, uint numMips = 1, uint numSlices = 1) : RHI::Texture(
		device, RHI::Resource::ResourceDesc::GetTextureBufferDesc(
			format, RHI::ResourceDimension::Texture2D, width, height, numMips, numSlices
		)
	) {};	
	const size_t mips() { return m_Desc.mipLevels; }
	const size_t slices() { return m_Desc.arraySize; }
	void* subresource(uint arraySlice, uint mipSlice) {
		uint subresource = m_Desc.indexSubresource(arraySlice, mipSlice);
		return Map(subresource);
	}
	void copy(void* srcData, uint srcPitch, uint srcHeight, uint dstArraySlice = 0, uint dstMipSlice = 0) {
		const D3D12_RESOURCE_DESC Desc = m_Desc;
		UINT SubresourceIndex = m_Desc.indexSubresource(dstArraySlice, dstMipSlice);
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT Layout;
		UINT NumRows;
		UINT64 RowSizeInBytes;
		UINT64 TotalBytes;
		m_Device->GetNativeDevice()->GetCopyableFootprints(&Desc, SubresourceIndex, 1, 0, &Layout, &NumRows, &RowSizeInBytes, &TotalBytes);
		D3D12_MEMCPY_DEST dest{
			.pData = subresource(dstArraySlice,dstMipSlice),
			.RowPitch = Layout.Footprint.RowPitch,
			.SlicePitch = 0
		};
		D3D12_SUBRESOURCE_DATA src{
			.pData = srcData,
			.RowPitch = srcPitch,
			.SlicePitch = 0
		};
		MemcpySubresource(&dest, &src, RowSizeInBytes, NumRows, 1);
	}
}; 