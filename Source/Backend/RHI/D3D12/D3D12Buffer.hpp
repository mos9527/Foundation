#pragma once
#include "D3D12Types.hpp"

#define RESOURCE_BARRIER_ALL_SUBRESOURCES D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES 
#define RAW_BUFFER_STRIDE 0u
namespace RHI {
	class Device;
	class CommandList;
	class Buffer : public DeviceChild {
	public:
		struct BufferDesc {
			ClearValue clearValue{};
			ResourceUsage usage{ ResourceUsage::Default };
			ResourceFormat format{ ResourceFormat::Unknown };
			ResourceDimension dimension{ ResourceDimension::Unknown };
			UINT64 alignment{ 0 };
			UINT64 stride{ 0 }; /* For Buffers, width / stride will be its No. of elements. For Raw Buffers, this value must be 0. */
			UINT64 width{ 0 }; /* For Buffers, this will be its size in bytes. For textures, this will be the image's dimension. */
			uint height{ 0 };
			UINT16 mipLevels{ 1 };
			UINT16 arraySize{ 1 };
			uint sampleCount{ 1 };
			uint sampleQuality{ 0 };
			TextureLayout layout{ TextureLayout::Unknown };
			ResourceFlags flags{ ResourceFlags::None };
			ResourceState initialState{ ResourceState::Common };
			ResourcePoolType poolType{ ResourcePoolType::Default };
			static const BufferDesc GetGenericBufferDesc(
				UINT64 size, 
				UINT64 stride = 0, /* Set to 0 for Raw Buffers. Non-zero values indicates a Structured Buffer */
				ResourceState initialState = ResourceState::CopySource,
				ResourceUsage usage = ResourceUsage::Upload, 
				ResourceFlags flags = ResourceFlags::None,
				ResourcePoolType poolType = ResourcePoolType::Default
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
					.initialState = initialState,
					.poolType = poolType
				};
			}
			static const BufferDesc GetTextureBufferDesc(
				ResourceFormat format,
				ResourceDimension dimension,
				UINT64 width,
				uint height,
				UINT16 mipLevels,
				UINT16 arraySize,
				uint sampleCount = 1,
				uint sampleQuality = 0,
				ResourceFlags flags = ResourceFlags::None,
				ResourceUsage usage = ResourceUsage::Default,
				ResourceState initialState = ResourceState::CopyDest,
				ResourcePoolType poolType = ResourcePoolType::Default,
				ClearValue clearValue = ClearValue{}
			) {
				return BufferDesc{
					.clearValue = clearValue,
					.usage = usage,
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
					.initialState = initialState,
					.poolType = poolType
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
					.Format = ResourceFormatToD3DFormat(format),
					.SampleDesc = DXGI_SAMPLE_DESC{
						.Count = sampleCount,
						.Quality = sampleQuality
					},
					.Layout = (D3D12_TEXTURE_LAYOUT)layout,
					.Flags = (D3D12_RESOURCE_FLAGS)flags
				};
			}
			inline bool isRawBuffer() const {
				return stride == 0;
			}
			inline size_t sizeInBytes() const {
				CHECK(dimension == ResourceDimension::Buffer);
				return width;
			}
		};
		// creation w/ exisiting d3d12 buffer. i.e. a backbuffer.
		Buffer(Device* device, BufferDesc const& desc, ComPtr<ID3D12Resource>&& texture) : DeviceChild(device), m_Desc(desc),m_Resource(texture) {};
		// creation w/o any initial data
		Buffer(Device* device, BufferDesc const& desc);
		// creation w/ initial data
		Buffer(Device* device, BufferDesc const& desc, const void* data, size_t size, size_t offset = 0LL);
		~Buffer() = default;
		inline BufferDesc const& GetDesc() { return m_Desc;  }
		
		inline ResourceState GetState() { return m_State; }
		void SetState(CommandList* cmdList, ResourceState state, uint subresource = RESOURCE_BARRIER_ALL_SUBRESOURCES);
		
		inline auto GetGPUAddress() { return m_Resource->GetGPUVirtualAddress(); }
		inline auto GetNativeBuffer() { return m_Resource.Get(); }

		inline operator ID3D12Resource* () { return m_Resource.Get(); }
		inline void Reset() { m_Resource.Reset(); }		

		// Map and immediately update the buffer content (when usage is Upload & Readback)
		void Update(const void* data, size_t size, size_t offset);
		// Queues a copy from srcBuffer to this buffer on the command list
		void QueueCopy(CommandList* cmdList, Buffer* srcBuffer, size_t srcOffset, size_t dstOffset, size_t size);		
		void Map();
		void Unmap();		

		inline auto const& GetName() { return m_Name; }
		inline void SetName(name_t name) { 
			m_Name = name;
			m_Resource->SetName((const wchar_t*)name.c_str());
			if (m_Allocation.Get()) m_Allocation->SetName((const wchar_t*)name.c_str());
		}

	protected:
		name_t m_Name;

		const BufferDesc m_Desc;
		ResourceState m_State{ ResourceState::Common };
		ComPtr<ID3D12Resource> m_Resource;
		ComPtr<D3D12MA::Allocation> m_Allocation;	

		void* pMappedData{ nullptr };
	};
}