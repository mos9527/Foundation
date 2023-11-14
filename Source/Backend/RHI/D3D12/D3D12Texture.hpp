#pragma once
#include "../Common.hpp"
#include "D3D12Buffer.hpp"
namespace RHI {
	struct SubresourceData {
		const void* pSysMem;
		uint rowPitch;
		uint slicePitch;
		operator const D3D12_SUBRESOURCE_DATA() const {
			return D3D12_SUBRESOURCE_DATA{
				.pData = pSysMem,
				.RowPitch = rowPitch,
				.SlicePitch = slicePitch
			};
		}
	};
	class Device;
	class Texture : public Buffer {
	public:
		typedef Buffer::BufferDesc TextureDesc;
		using Buffer::operator ID3D12Resource*;

		Texture(Device* device, TextureDesc const& desc, ComPtr<ID3D12Resource>&& texture) : Buffer(device, desc, std::move(texture)) {};
		Texture(Device* device, TextureDesc const& desc);
		Texture(Device* device, TextureDesc const& desc, CommandList* cmdList, SubresourceData* data, uint count);
		~Texture() = default;
	protected:
		using Buffer::m_Name;
		using Buffer::m_Desc;
		using Buffer::m_State;
		using Buffer::m_Resource;
		using Buffer::m_Allocation;
		using Buffer::pMappedData;
	};
}