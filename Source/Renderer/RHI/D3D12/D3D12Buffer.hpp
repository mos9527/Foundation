#pragma once
#include "../Common.hpp"
#include "D3D12Resource.hpp"
#define RESOURCE_BARRIER_ALL_SUBRESOURCES D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES 
namespace RHI {
	class Device;
	class CommandList;
	class Buffer {
	public:
		struct BufferDesc {
			ClearValue clearValue{};
			ResourceUsage usage{ ResourceUsage::Default };
			ResourceFormat format{ ResourceFormat::Unknown };
			ResourceDimension dimension{ ResourceDimension::Unknown };
			UINT64 alignment{ 0 };
			UINT64 stride{ 0 };
			UINT64 width{ 0 };
			UINT height{ 0 };
			UINT16 mipLevels{ 1 };
			UINT16 arraySize{ 1 };
			UINT sampleCount{ 1 };
			UINT sampleQuality{ 0 };
			TextureLayout layout{ TextureLayout::Unknown };
			ResourceFlags flags{ ResourceFlags::None };
			ResourceState initialState{ ResourceState::Common };
			static const BufferDesc GetGenericBufferDesc(
				UINT64 size, 
				UINT64 stride = sizeof(size_t),
				ResourceState initialState = ResourceState::CopySource,
				ResourceUsage usage = ResourceUsage::Upload, 
				ResourceFlags flags = ResourceFlags::None
			) {
				return BufferDesc{
					.usage = usage,
					.format = ResourceFormat::Unknown,
					.dimension = ResourceDimension::Buffer,
					.stride = stride,
					.width = size,
					.height = 1,
					.mipLevels = 1,
					.arraySize = 1,
					.sampleCount = 1,
					.sampleQuality = 0,
					.layout = TextureLayout::RowMajor,
					.flags = flags,
					.initialState = initialState
				};
			}
			static const BufferDesc GetTextureBufferDesc(
				ResourceFormat format,
				ResourceDimension dimension,
				UINT64 width,
				UINT height,
				UINT16 mipLevels,
				UINT16 arraySize,
				UINT sampleCount = 1,
				UINT sampleQuality = 0,
				ResourceFlags flags = ResourceFlags::None,
				ResourceUsage usage = ResourceUsage::Default,
				ResourceState initialState = ResourceState::CopyDest
			) {
				return BufferDesc{
					.format = format,
					.dimension = dimension,					
					.width = width,
					.height = height,
					.mipLevels = mipLevels,
					.arraySize = arraySize,
					.sampleCount = sampleCount,
					.sampleQuality = sampleQuality,
					.layout = TextureLayout::Unknown,
					.flags = flags,
					.initialState = initialState
				};
			}
			operator const D3D12_RESOURCE_DESC() const {
				return D3D12_RESOURCE_DESC{
					.Dimension = (D3D12_RESOURCE_DIMENSION)dimension,
					.Alignment = alignment,
					.Width = width,
					.Height = height,
					.DepthOrArraySize = arraySize,
					.MipLevels = mipLevels,
					.Format = (DXGI_FORMAT)format,
					.SampleDesc = DXGI_SAMPLE_DESC{
						.Count = sampleCount,
						.Quality = sampleQuality
					},
					.Layout = (D3D12_TEXTURE_LAYOUT)layout,
					.Flags = (D3D12_RESOURCE_FLAGS)flags
				};
			}
		};
		// creation w/ descriptor only. useful for inherited classes
		Buffer(BufferDesc const& desc) : m_Desc(desc) {};
		// creation w/ exisiting d3d12 buffer. i.e. a backbuffer.
		Buffer(BufferDesc const& desc, ComPtr<ID3D12Resource>&& texture) : m_Desc(desc),m_Resource(texture) {};
		// creation w/o any initial data
		Buffer(Device* device, BufferDesc const& desc);
		// creation w/ initial data
		Buffer(Device* device, BufferDesc const& desc, const void* data, size_t size, size_t offset = 0LL);
		~Buffer() = default;
		inline ResourceState GetState() { return m_State; }
		void SetState(CommandList* cmdList, ResourceState state, UINT subresource = RESOURCE_BARRIER_ALL_SUBRESOURCES);
		inline auto GetNativeBuffer() { return m_Resource.Get(); }
		inline operator ID3D12Resource* () { return m_Resource.Get(); }
		inline void Reset() { m_Resource.Reset(); }
		/* Buffer dimension exclusive (since these only handle subresource #0) */
		// Map and immediately update the buffer content (when usage is Upload & Readback)
		void Update(const void* data, size_t size, size_t offset);
		// Queues a copy from srcBuffer to this buffer on the command list
		void QueueCopy(CommandList* cmdList, Buffer* srcBuffer, size_t srcOffset, size_t dstOffset, size_t size);		
		void Map();
		void Unmap();
	protected:
		const BufferDesc m_Desc;
		ResourceState m_State{ ResourceState::Common };
		ComPtr<ID3D12Resource> m_Resource;
		ComPtr<D3D12MA::Allocation> m_Allocation;
		void* pMappedData{ nullptr };
	};
}